[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression

function Write-CsprngFile
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][UInt64]$Length
    )
    $stream = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, 1MB,
        [System.IO.FileOptions]::WriteThrough)
    $buffer = [byte[]]::new(1MB)
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try
    {
        $remaining = $Length
        while ($remaining -gt 0)
        {
            $count = [int][Math]::Min([UInt64]$buffer.Length, $remaining)
            $random.GetBytes($buffer)
            $stream.Write($buffer, 0, $count)
            $remaining -= [UInt64]$count
        }
        $stream.Flush($true)
    }
    finally
    {
        [Array]::Clear($buffer, 0, $buffer.Length)
        $random.Dispose()
        $stream.Dispose()
    }
}

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

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
$sourceSetName = [System.IO.Path]::GetFileName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or [string]::IsNullOrWhiteSpace($sourceSetName) -or
    -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Source-set parent does not exist or output name is invalid: $resolvedOutput"
}
$stagingDirectory = Join-Path $outputParent "$sourceSetName.partial"
$sealPath = Join-Path $outputParent "$sourceSetName.seal.json"
$temporarySealPath = Join-Path $outputParent "$sourceSetName.partial.seal.json"
foreach ($path in @($resolvedOutput, $stagingDirectory, $sealPath, $temporarySealPath))
{
    if (Test-Path -LiteralPath $path)
    {
        throw "Create-only source-set output already exists: $path"
    }
}

$sourceSetIdBytes = [byte[]]::new(16)
$random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try
{
    $random.GetBytes($sourceSetIdBytes)
}
finally
{
    $random.Dispose()
}
$sourceSetId = ([BitConverter]::ToString($sourceSetIdBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($sourceSetIdBytes, 0, $sourceSetIdBytes.Length)
if ($sourceSetId -notmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid 128-bit sourceSetId'
}

[void](New-Item -ItemType Directory -Path $stagingDirectory)
$published = $false
try
{
    $oneMebibytePath = Join-Path $stagingDirectory 'random-1MiB.bin'
    $eightMebibytePath = Join-Path $stagingDirectory 'random-8MiB.bin'
    $zipPayloadPath = Join-Path $stagingDirectory 'zip-payload-4MiB.bin.partial'
    $zipPath = Join-Path $stagingDirectory 'random-payload-4MiB.zip'
    Write-CsprngFile -Path $oneMebibytePath -Length 1MB
    Write-CsprngFile -Path $eightMebibytePath -Length 8MB
    Write-CsprngFile -Path $zipPayloadPath -Length 4MB
    $zipPayloadSha256 = Get-FileSha256 -Path $zipPayloadPath

    $zipStream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $archive = [System.IO.Compression.ZipArchive]::new($zipStream,
            [System.IO.Compression.ZipArchiveMode]::Create, $true)
        try
        {
            $entry = $archive.CreateEntry('payload.bin', [System.IO.Compression.CompressionLevel]::Optimal)
            $entryStream = $entry.Open()
            $sourceStream = [System.IO.File]::OpenRead($zipPayloadPath)
            try
            {
                $sourceStream.CopyTo($entryStream, 1MB)
            }
            finally
            {
                $sourceStream.Dispose()
                $entryStream.Dispose()
            }
        }
        finally
        {
            $archive.Dispose()
        }
        $zipStream.Flush($true)
    }
    finally
    {
        $zipStream.Dispose()
    }
    [System.IO.File]::Delete($zipPayloadPath)
    $zipLength = [UInt64](Get-Item -LiteralPath $zipPath).Length
    if ($zipLength -eq 0 -or $zipLength -gt 8MB)
    {
        throw 'ZIP fixture is empty or exceeds the 8 MiB single-Segment limit'
    }

    $files = @($oneMebibytePath, $eightMebibytePath, $zipPath) | ForEach-Object {
        $item = Get-Item -LiteralPath $_
        [ordered]@{
            path = $item.Name
            size = [UInt64]$item.Length
            sha256 = Get-FileSha256 -Path $item.FullName
            pixelBridgeSegmentCompression = 'RAW/OFF'
        }
    }
    $files = @($files | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
    $sourceSetFingerprint = Get-CanonicalSourceFingerprint -Files $files
    $manifest = [ordered]@{
        schema = 'PixelBridge.RemoteVisualSourceSet.2'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        sourceSetName = $sourceSetName
        sourceSetId = $sourceSetId
        generator = 'Windows OS CSPRNG via RandomNumberGenerator.GetBytes'
        sourceSetFingerprintSha256 = $sourceSetFingerprint
        fileCount = $files.Count
        files = $files
        zipSourcePayload = [ordered]@{
            archivePath = 'random-payload-4MiB.zip'
            entryPath = 'payload.bin'
            size = [UInt64]4MB
            sha256 = $zipPayloadSha256
        }
    }
    $manifestPath = Join-Path $stagingDirectory 'source-manifest.json'
    Write-NewUtf8File -Path $manifestPath -Content ($manifest | ConvertTo-Json -Depth 10)
    $seal = [ordered]@{
        schema = 'PixelBridge.RemoteVisualSourceSetSeal.2'
        sourceSetName = $sourceSetName
        sourceSetId = $sourceSetId
        sourceSetFingerprintSha256 = $sourceSetFingerprint
        manifest = [ordered]@{
            path = 'source-manifest.json'
            size = [UInt64](Get-Item -LiteralPath $manifestPath).Length
            sha256 = Get-FileSha256 -Path $manifestPath
        }
    }
    Write-NewUtf8File -Path $temporarySealPath -Content ($seal | ConvertTo-Json -Depth 8)

    Move-Item -LiteralPath $stagingDirectory -Destination $resolvedOutput
    Move-Item -LiteralPath $temporarySealPath -Destination $sealPath
    $verificationScript = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
    if (-not (Test-Path -LiteralPath $verificationScript -PathType Leaf))
    {
        throw "Source-set verifier is missing: $verificationScript"
    }
    $verification = & $verificationScript -SourceSetDirectory $resolvedOutput -SourceSealPath $sealPath
    $published = $true
    [ordered]@{
        sourceSetDirectory = $resolvedOutput
        sealPath = $sealPath
        sourceSetId = $sourceSetId
        manifestSha256 = $seal.manifest.sha256
        sourceSetFingerprintSha256 = $sourceSetFingerprint
        verification = $verification
    } | ConvertTo-Json -Depth 12
}
finally
{
    if (-not $published)
    {
        foreach ($path in @($stagingDirectory, $temporarySealPath, $resolvedOutput, $sealPath))
        {
            if (Test-Path -LiteralPath $path)
            {
                if (Test-Path -LiteralPath $path -PathType Container)
                {
                    [System.IO.Directory]::Delete([System.IO.Path]::GetFullPath($path), $true)
                }
                else
                {
                    [System.IO.File]::Delete([System.IO.Path]::GetFullPath($path))
                }
            }
        }
    }
}
