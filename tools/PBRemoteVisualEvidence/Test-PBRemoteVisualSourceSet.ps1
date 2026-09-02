[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceSetDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourceSealPath,

    [string]$ExpectedManifestSha256,

    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try
        {
            $writer.Write($Content)
            $writer.Flush()
            $stream.Flush($true)
        }
        finally
        {
            $writer.Dispose()
        }
    }
    finally
    {
        $stream.Dispose()
    }
}

function Get-FileSha256
{
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-StreamSha256
{
    param([Parameter(Mandatory = $true)][System.IO.Stream]$Stream)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($algorithm.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant()
    }
    finally
    {
        $algorithm.Dispose()
    }
}

function Get-TextSha256
{
    param([Parameter(Mandatory = $true)][string]$Text)
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally
    {
        $algorithm.Dispose()
    }
}

function Get-CanonicalSourceFingerprint
{
    param([Parameter(Mandatory = $true)][object[]]$Files)
    $sortedFiles = @($Files | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
    $canonical = ($sortedFiles | ForEach-Object { "$($_.path)`0$($_.size)`0$($_.sha256)`n" }) -join ''
    return Get-TextSha256 -Text $canonical
}

function Read-BoundedJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [UInt64]$MaximumBytes = 1MB
    )
    $item = Get-Item -LiteralPath $Path
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.Length -eq 0 -or
        [UInt64]$item.Length -gt $MaximumBytes)
    {
        throw "JSON artifact is empty, oversized, or a reparse point: $Path"
    }
    try
    {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable -Depth 40
    }
    catch
    {
        throw "JSON artifact is malformed: $Path ($($_.Exception.Message))"
    }
}

function Require-Hash
{
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Value -isnot [string] -or $Value -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name must be 64 lowercase hexadecimal characters"
    }
}

function Test-RelativePath
{
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Length -gt 255 -or $Path.Contains('\') -or
        $Path.StartsWith('/') -or $Path.Contains(':') -or $Path.Contains("`0") -or $Path.Contains('/'))
    {
        return $false
    }
    return $Path -ne '.' -and $Path -ne '..'
}

$resolvedSourceSet = [System.IO.Path]::GetFullPath($SourceSetDirectory)
$resolvedSeal = [System.IO.Path]::GetFullPath($SourceSealPath)
if (-not (Test-Path -LiteralPath $resolvedSourceSet -PathType Container) -or
    -not (Test-Path -LiteralPath $resolvedSeal -PathType Leaf))
{
    throw 'Source-set directory or seal does not exist'
}
foreach ($item in @((Get-Item -LiteralPath $resolvedSourceSet), (Get-Item -LiteralPath $resolvedSeal)))
{
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Source-set input cannot be a reparse point: $($item.FullName)"
    }
}
$directoryReparse = Get-ChildItem -LiteralPath $resolvedSourceSet -Directory -Recurse -Force | Where-Object {
    ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
} | Select-Object -First 1
if ($null -ne $directoryReparse)
{
    throw "Source set contains a directory reparse point: $($directoryReparse.FullName)"
}

$manifestPath = Join-Path $resolvedSourceSet 'source-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf))
{
    throw "Source manifest is missing: $manifestPath"
}
$manifest = Read-BoundedJson -Path $manifestPath
$seal = Read-BoundedJson -Path $resolvedSeal
$manifestSha256 = Get-FileSha256 -Path $manifestPath
if ($manifest.schema -cne 'PixelBridge.RemoteVisualSourceSet.2' -or
    $seal.schema -cne 'PixelBridge.RemoteVisualSourceSetSeal.2')
{
    throw 'Source-set manifest or seal schema mismatch'
}
if ($manifest.sourceSetName -cne (Split-Path -Leaf $resolvedSourceSet) -or
    $manifest.sourceSetId -cnotmatch '^[0-9a-f]{32}$' -or
    $manifest.generator -cne 'Windows OS CSPRNG via RandomNumberGenerator.GetBytes')
{
    throw 'Source-set name or 128-bit identity is invalid'
}
Require-Hash -Value $manifest.sourceSetFingerprintSha256 -Name 'sourceSetFingerprintSha256'
Require-Hash -Value $manifestSha256 -Name 'manifest SHA-256'
if ($ExpectedManifestSha256 -and
    ($ExpectedManifestSha256 -cnotmatch '^[0-9a-fA-F]{64}$' -or
    $manifestSha256 -cne $ExpectedManifestSha256.ToLowerInvariant()))
{
    throw 'Source manifest SHA-256 does not match the expected deployed identity'
}
$manifestItem = Get-Item -LiteralPath $manifestPath
if ($seal.sourceSetName -cne $manifest.sourceSetName -or $seal.sourceSetId -cne $manifest.sourceSetId -or
    $seal.sourceSetFingerprintSha256 -cne $manifest.sourceSetFingerprintSha256 -or
    $seal.manifest.path -cne 'source-manifest.json' -or
    [UInt64]$seal.manifest.size -ne [UInt64]$manifestItem.Length -or
    $seal.manifest.sha256 -cne $manifestSha256)
{
    throw 'Source-set seal does not bind the exact manifest and source identity'
}

$files = @($manifest.files)
if ([UInt64]$manifest.fileCount -ne 3 -or $files.Count -ne 3)
{
    throw 'Source set must contain exactly three payload files'
}
$expectedSizes = @{
    'random-1MiB.bin' = [UInt64]1MB
    'random-8MiB.bin' = [UInt64]8MB
    'random-payload-4MiB.zip' = $null
}
$expectedPaths = @('random-1MiB.bin', 'random-8MiB.bin', 'random-payload-4MiB.zip')
$fileMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $files)
{
    $path = [string]$entry.path
    if (-not (Test-RelativePath -Path $path) -or -not ($expectedPaths -ccontains $path) -or
        $fileMap.ContainsKey($path) -or $entry.pixelBridgeSegmentCompression -cne 'RAW/OFF')
    {
        throw "Invalid, duplicate, or unexpected source-set path: $path"
    }
    Require-Hash -Value $entry.sha256 -Name "source file hash $path"
    if ($null -ne $expectedSizes[$path] -and [UInt64]$entry.size -ne [UInt64]$expectedSizes[$path])
    {
        throw "Source fixture has the wrong fixed size: $path"
    }
    if ($path -ceq 'random-payload-4MiB.zip' -and ([UInt64]$entry.size -eq 0 -or [UInt64]$entry.size -gt 8MB))
    {
        throw 'ZIP fixture is empty or exceeds 8 MiB'
    }
    $fileMap.Add($path, $entry)
}
if ((Get-CanonicalSourceFingerprint -Files $files) -cne $manifest.sourceSetFingerprintSha256)
{
    throw 'Source-set payload fingerprint does not match its manifest'
}

$actualDirectories = @(Get-ChildItem -LiteralPath $resolvedSourceSet -Directory -Recurse -Force)
if ($actualDirectories.Count -ne 0)
{
    throw 'Source-set directory must be flat and cannot contain extra directories'
}
$actualFiles = @(Get-ChildItem -LiteralPath $resolvedSourceSet -File -Recurse -Force)
if ($actualFiles.Count -ne 4)
{
    throw "Source-set directory must contain three payload files and one manifest, found $($actualFiles.Count) files"
}
$rootPrefix = $resolvedSourceSet.TrimEnd('\') + '\'
foreach ($file in $actualFiles)
{
    $fullPath = [System.IO.Path]::GetFullPath($file.FullName)
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Source-set file escaped the root or is a reparse point: $fullPath"
    }
    $relativePath = $fullPath.Substring($rootPrefix.Length).Replace('\', '/')
    if ($relativePath -ceq 'source-manifest.json')
    {
        continue
    }
    if (-not $fileMap.ContainsKey($relativePath))
    {
        throw "Unmanifested source-set file: $relativePath"
    }
    $expected = $fileMap[$relativePath]
    if ([UInt64]$file.Length -ne [UInt64]$expected.size -or
        (Get-FileSha256 -Path $fullPath) -cne $expected.sha256)
    {
        throw "Source-set file identity mismatch: $relativePath"
    }
}

$zipBinding = $manifest.zipSourcePayload
if ($zipBinding.archivePath -cne 'random-payload-4MiB.zip' -or $zipBinding.entryPath -cne 'payload.bin' -or
    [UInt64]$zipBinding.size -ne 4MB)
{
    throw 'ZIP inner-payload binding is invalid'
}
Require-Hash -Value $zipBinding.sha256 -Name 'ZIP inner-payload hash'
$zipFilePath = Join-Path $resolvedSourceSet 'random-payload-4MiB.zip'
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipFilePath)
try
{
    $fileEntries = @($archive.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
    if ($archive.Entries.Count -ne 1 -or $fileEntries.Count -ne 1 -or $fileEntries[0].FullName -cne 'payload.bin' -or
        [UInt64]$fileEntries[0].Length -ne 4MB)
    {
        throw 'ZIP must contain exactly one 4 MiB payload.bin entry'
    }
    $entryStream = $fileEntries[0].Open()
    try
    {
        $innerSha256 = Get-StreamSha256 -Stream $entryStream
    }
    finally
    {
        $entryStream.Dispose()
    }
}
finally
{
    $archive.Dispose()
}
if ($innerSha256 -cne $zipBinding.sha256)
{
    throw 'ZIP inner payload SHA-256 mismatch'
}

$result = [ordered]@{
    schema = 'PixelBridge.RemoteVisualSourceSetVerification.1'
    verified = $true
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    sourceSetDirectory = $resolvedSourceSet
    sealPath = $resolvedSeal
    sourceSetId = $manifest.sourceSetId
    manifest = [ordered]@{
        path = $manifestPath
        size = [UInt64]$manifestItem.Length
        sha256 = $manifestSha256
    }
    sourceSetFingerprintSha256 = $manifest.sourceSetFingerprintSha256
    fileCount = $files.Count
}
$json = $result | ConvertTo-Json -Depth 8
if ($OutputPath)
{
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
    $temporaryOutput = "$resolvedOutput.partial"
    if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
    {
        throw "Create-only verification output or partial already exists: $resolvedOutput"
    }
    $outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
    if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
    {
        throw "Verification output parent does not exist: $outputParent"
    }
    try
    {
        Write-NewUtf8File -Path $temporaryOutput -Content ($json + "`n")
        Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryOutput)
        }
    }
}
$json
