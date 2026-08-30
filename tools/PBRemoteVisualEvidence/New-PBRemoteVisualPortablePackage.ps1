[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Encoder', 'Decoder')]
    [string]$Role,

    [Parameter(Mandatory = $true)]
    [ValidateSet('current-head', 'instrumented-pre-hardening', 'final-hardening')]
    [string]$Label,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [string[]]$ExcludedSourcePath = @('docs/PHASE1_GATE_REPORT.md')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, $encoding)
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

function Get-CanonicalTextSha256
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

function Get-FileInventory
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Files
    )
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    return @($Files | Sort-Object FullName | ForEach-Object {
        $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
        if (-not $fullPath.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Inventory path escaped root: $fullPath"
        }
        [ordered]@{
            path = $fullPath.Substring($resolvedRoot.Length).Replace('\', '/')
            size = [UInt64]$_.Length
            sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot '.git') -PathType Container))
{
    throw "Repository root is not a Git worktree: $repositoryRoot"
}
if (-not (Test-Path -LiteralPath $resolvedBuild -PathType Container))
{
    throw "Build directory does not exist: $resolvedBuild"
}
if (-not (Test-Path -LiteralPath $resolvedOutputRoot -PathType Container))
{
    [void](New-Item -ItemType Directory -Path $resolvedOutputRoot)
}

$applicationName = "PixelBridge$Role"
$applicationDirectory = Join-Path $resolvedBuild "apps\$applicationName\Release"
$applicationExecutable = Join-Path $applicationDirectory "$applicationName.exe"
if (-not (Test-Path -LiteralPath $applicationExecutable -PathType Leaf))
{
    throw "Release executable does not exist: $applicationExecutable"
}

$headCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $headCommit -notmatch '^[0-9a-f]{40}$')
{
    throw 'Unable to resolve HEAD'
}
$tagObject = (& git -C $repositoryRoot rev-parse phase1-gate-pass).Trim()
$tagCommit = (& git -C $repositoryRoot rev-parse 'phase1-gate-pass^{}').Trim()
if ($LASTEXITCODE -ne 0)
{
    throw 'Unable to resolve phase1-gate-pass identity'
}

$sourcePaths = @(& git -C $repositoryRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0)
{
    throw 'Unable to enumerate source tree'
}
$excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($path in $ExcludedSourcePath)
{
    [void]$excluded.Add($path.Replace('\', '/'))
}
$sourceInventory = @($sourcePaths | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and -not $excluded.Contains($_.Replace('\', '/'))
} | Sort-Object | ForEach-Object {
    $relativePath = $_.Replace('\', '/')
    if ($relativePath.Contains("`r") -or $relativePath.Contains("`n"))
    {
        throw "Unsupported source path contains a line break: $relativePath"
    }
    $absolutePath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $_))
    $repositoryPrefix = $repositoryRoot.TrimEnd('\') + '\'
    if (-not $absolutePath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $absolutePath -PathType Leaf))
    {
        throw "Source inventory path is invalid: $relativePath"
    }
    [ordered]@{
        path = $relativePath
        size = [UInt64](Get-Item -LiteralPath $absolutePath).Length
        sha256 = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$sourceCanonical = ($sourceInventory | ForEach-Object { "$($_.path)`0$($_.size)`0$($_.sha256)`n" }) -join ''
$sourceFingerprint = Get-CanonicalTextSha256 -Text $sourceCanonical

$packageName = "PixelBridge-P1_5-$Label-$Role-$($headCommit.Substring(0, 8))-$($sourceFingerprint.Substring(0, 12))"
$packageDirectory = Join-Path $resolvedOutputRoot $packageName
$zipPath = Join-Path $resolvedOutputRoot "$packageName.zip"
$temporaryZipPath = Join-Path $resolvedOutputRoot "$packageName.partial.zip"
$sealPath = Join-Path $resolvedOutputRoot "$packageName.seal.json"
foreach ($path in @($packageDirectory, $zipPath, $temporaryZipPath, $sealPath))
{
    if (Test-Path -LiteralPath $path)
    {
        throw "Create-only output already exists: $path"
    }
}
[void](New-Item -ItemType Directory -Path $packageDirectory)

Get-ChildItem -LiteralPath $applicationDirectory -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $packageDirectory -Recurse
}
if (-not (Test-Path -LiteralPath (Join-Path $packageDirectory "$applicationName.exe") -PathType Leaf))
{
    throw 'Copied package is missing its executable'
}

$packageFiles = @(Get-ChildItem -LiteralPath $packageDirectory -File -Recurse)
if ($packageFiles.Count -eq 0 -or $packageFiles.Count -gt 4096)
{
    throw "Package file inventory is outside 1..4096: $($packageFiles.Count)"
}
$packageBytes = [UInt64](($packageFiles | Measure-Object -Property Length -Sum).Sum)
if ($packageBytes -gt 2GB)
{
    throw "Package exceeds the 2 GiB evidence bound: $packageBytes"
}
$packageInventory = Get-FileInventory -Root $packageDirectory -Files $packageFiles

$cachePath = Join-Path $resolvedBuild 'CMakeCache.txt'
$cacheSha256 = $null
if (Test-Path -LiteralPath $cachePath -PathType Leaf)
{
    $cacheSha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest = [ordered]@{
    schema = 'PixelBridge.PortablePackage.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    label = $Label
    endpointRole = $Role
    application = $applicationName
    headCommit = $headCommit
    phase1GatePassTagObject = $tagObject
    phase1GatePassCommit = $tagCommit
    testedSourceFingerprintSha256 = $sourceFingerprint
    excludedUnrelatedSourcePaths = @($ExcludedSourcePath)
    buildDirectory = $resolvedBuild
    cmakeCacheSha256 = $cacheSha256
    cmakeVersion = (& cmake --version | Select-Object -First 1)
    gitVersion = (& git --version)
    packageFileCount = $packageFiles.Count
    packagePayloadBytes = $packageBytes
    files = $packageInventory
    sourceFiles = $sourceInventory
}
$manifestPath = Join-Path $packageDirectory 'package-manifest.json'
Write-NewUtf8File -Path $manifestPath -Content ($manifest | ConvertTo-Json -Depth 12)

Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $packageDirectory -Force).FullName `
    -DestinationPath $temporaryZipPath -CompressionLevel Optimal
Move-Item -LiteralPath $temporaryZipPath -Destination $zipPath
$seal = [ordered]@{
    schema = 'PixelBridge.PortablePackageSeal.1'
    packageName = $packageName
    endpointRole = $Role
    label = $Label
    headCommit = $headCommit
    testedSourceFingerprintSha256 = $sourceFingerprint
    archive = [ordered]@{
        path = [System.IO.Path]::GetFileName($zipPath)
        size = [UInt64](Get-Item -LiteralPath $zipPath).Length
        sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    manifest = [ordered]@{
        path = "$packageName/package-manifest.json"
        size = [UInt64](Get-Item -LiteralPath $manifestPath).Length
        sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
Write-NewUtf8File -Path $sealPath -Content ($seal | ConvertTo-Json -Depth 8)
$seal | ConvertTo-Json -Depth 8
