#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$SealPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedManifestSha256,
    [string]$ArchivePath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$maximumBytes = 512MB

function Assert-UnlinkedPath
{
    param([string]$Path)
    $current = [System.IO.Path]::GetFullPath($Path)
    while ($current)
    {
        if (Test-Path -LiteralPath $current)
        {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse path rejected' }
        }
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}

function Get-Identity
{
    param([string]$Path)
    Assert-UnlinkedPath $Path
    $file = Get-Item -LiteralPath $Path -Force
    if ($file.PSIsContainer -or $file.Length -gt $maximumBytes) { throw 'File size/type rejected' }
    $size = $file.Length
    $modified = $file.LastWriteTimeUtc
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $after = Get-Item -LiteralPath $Path -Force
    if ($after.Length -ne $size -or $after.LastWriteTimeUtc -ne $modified) { throw 'File changed during verification' }
    return @{ size = [long]$size; sha256 = $hash }
}

function Read-BoundedJson
{
    param([string]$Path)
    Assert-UnlinkedPath $Path
    if ((Get-Item -LiteralPath $Path).Length -gt 8MB) { throw 'JSON exceeds limit' }
    return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
}

$root = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\', '/')
Assert-UnlinkedPath $root
$manifestPath = Join-Path $root 'package-manifest.json'
$identity = Get-Identity $manifestPath
if ($identity.sha256 -cne $ExpectedManifestSha256) { throw 'Pinned manifest hash mismatch' }
$manifest = Read-BoundedJson $manifestPath
$seal = Read-BoundedJson $SealPath
if ($manifest.schema -cne 'PixelBridge.Step1.ExperimentalPackage.1' -or $manifest.candidate -cne 'M1' -or
    $manifest.cleanRelease -ne $false -or $manifest.fieldStatus -cne 'NOT_RUN' -or
    $manifest.baseCommit -cne '4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a' -or
    $seal.schema -cne 'PixelBridge.Step1.ExperimentalPackageSeal.1' -or
    $seal.manifest.sha256 -cne $identity.sha256 -or $seal.manifest.size -ne $identity.size -or
    $seal.sourceFingerprintSha256 -cne $manifest.sourceFingerprintSha256) { throw 'Experiment identity/seal mismatch' }
$expected = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$total = [long]0
foreach ($entry in @($manifest.files))
{
    $relative = [string]$entry.path
    if ($relative.Length -eq 0 -or $relative.Length -gt 240 -or $relative -match '[\\:<>"|?*\x00-\x1f]' -or $relative.StartsWith('/')) { throw 'Invalid relative path' }
    foreach ($part in $relative.Split('/'))
    {
        if ($part -in @('', '.', '..') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') { throw 'Ambiguous relative path' }
    }
    if (!$expected.Add($relative)) { throw 'Duplicate case-folded path' }
    if ($expected.Count -gt 65536 -or $entry.size -lt 0 -or $entry.size -gt 128MB -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'File inventory limit' }
    $total += [long]$entry.size
    if ($total -gt $maximumBytes) { throw 'Aggregate byte limit' }
    $path = [System.IO.Path]::GetFullPath((Join-Path $root $relative))
    if (!$path.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Path escaped package root' }
    $actual = Get-Identity $path
    if ($actual.size -ne $entry.size -or $actual.sha256 -cne $entry.sha256) { throw "Payload mismatch: $relative" }
}
$pending = [System.Collections.Generic.Queue[string]]::new()
$pending.Enqueue($root)
$observed = 0
$directories = 0
while ($pending.Count -gt 0)
{
    $directory = $pending.Dequeue()
    $directories++
    if ($directories -gt 65536 -or ($directory.Substring($root.Length).Split('\').Count -gt 16)) { throw 'Directory traversal limit' }
    foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force))
    {
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse entry rejected' }
        if ($item.PSIsContainer)
        {
            if ($pending.Count -gt 65536) { throw 'Directory count limit' }
            $pending.Enqueue($item.FullName)
        }
        else
        {
            $relative = $item.FullName.Substring($root.Length + 1).Replace('\', '/')
            if ($relative -ceq 'package-manifest.json') { continue }
            if (!$expected.Contains($relative)) { throw "Unsealed extra file: $relative" }
            $observed++
        }
    }
}
if ($observed -ne $expected.Count) { throw 'Inventory coverage mismatch' }
$sourceHash = (Get-Identity (Join-Path $root 'source/source-inventory.json')).sha256
if ($sourceHash -cne $manifest.sourceFingerprintSha256) { throw 'Source fingerprint mismatch' }
if (@($manifest.applications).Count -ne 2) { throw 'Expected exactly two application identities' }
$roles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($application in @($manifest.applications))
{
    $role = [string]$application.role
    if ($role -cnotin @('Encoder', 'Decoder') -or !$roles.Add($role) -or
        $application.path -cne ($role + '/PixelBridge' + $role + '.exe') -or
        $application.identity.schema -cne 'PixelBridge.Step1.MeasurementBuildIdentity.1' -or
        $application.identity.candidate -cne 'M1' -or $application.identity.authority -cne 'InstrumentedExperiment' -or
        $application.identity.baseCommit -cne $manifest.baseCommit -or
        $application.identity.sourceFingerprintSha256 -cne $sourceHash -or
        $application.identity.wireChanged -ne $false -or $application.identity.profileChanged -ne $false) { throw 'Application identity mismatch' }
    $actual = Get-Identity (Join-Path $root $application.path)
    if ($actual.sha256 -cne $application.sha256 -or $actual.size -ne $application.size) { throw 'Application hash mismatch' }
}
$profileHash = (Get-Identity (Join-Path $root 'unified-profile.json')).sha256
if ($profileHash -cne '312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b' -or
    $manifest.profileSha256 -cne $profileHash) { throw 'Profile mismatch' }
if ($ArchivePath)
{
    $archive = Get-Identity $ArchivePath
    if ($archive.sha256 -cne $seal.archive.sha256 -or $archive.size -ne $seal.archive.size) { throw 'Archive seal mismatch' }
}
[ordered]@{ schema = 'PixelBridge.Step1.PortablePackageVerification.1'; verified = $true; packagedExecutablesRun = $false;
    authenticityClaim = $false; archiveMemberValidation = 'Use step1.py verify-package for bounded ZIP member validation';
    sourceFingerprintSha256 = $sourceHash; files = $observed; bytes = $total } | ConvertTo-Json
