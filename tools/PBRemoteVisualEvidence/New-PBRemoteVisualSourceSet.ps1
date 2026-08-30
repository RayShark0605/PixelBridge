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
    param([string]$Path, [string]$Content)
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

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $resolvedOutput)
{
    throw "Create-only source-set directory already exists: $resolvedOutput"
}
[void](New-Item -ItemType Directory -Path $resolvedOutput)

$oneMebibytePath = Join-Path $resolvedOutput 'random-1MiB.bin'
$eightMebibytePath = Join-Path $resolvedOutput 'random-8MiB.bin'
$zipPayloadPath = Join-Path $resolvedOutput 'zip-payload-4MiB.bin'
$zipPath = Join-Path $resolvedOutput 'random-payload-4MiB.zip'
Write-CsprngFile -Path $oneMebibytePath -Length 1MB
Write-CsprngFile -Path $eightMebibytePath -Length 8MB
Write-CsprngFile -Path $zipPayloadPath -Length 4MB

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
if ((Get-Item -LiteralPath $zipPath).Length -gt 8MB)
{
    throw 'ZIP fixture unexpectedly exceeds the current 8 MiB single-Segment limit'
}

$files = @($oneMebibytePath, $eightMebibytePath, $zipPath) | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [ordered]@{
        name = $item.Name
        size = [UInt64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        pixelBridgeSegmentCompression = 'RAW/OFF'
    }
}
$payloadItem = Get-Item -LiteralPath $zipPayloadPath
$manifest = [ordered]@{
    schema = 'PixelBridge.RemoteVisualSourceSet.1'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    generator = 'Windows OS CSPRNG via RandomNumberGenerator.Fill'
    files = $files
    zipSourcePayload = [ordered]@{
        name = $payloadItem.Name
        size = [UInt64]$payloadItem.Length
        sha256 = (Get-FileHash -LiteralPath $payloadItem.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifestPath = Join-Path $resolvedOutput 'source-manifest.json'
Write-NewUtf8File -Path $manifestPath -Content ($manifest | ConvertTo-Json -Depth 8)
$manifest | ConvertTo-Json -Depth 8
