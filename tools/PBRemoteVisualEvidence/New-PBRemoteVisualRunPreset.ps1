[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 128)]
    [string]$RemoteProvider,

    [string]$RemoteProviderVersion = '',

    [string]$RemoteMode = 'HighestAvailableQuality',

    [ValidateRange(1.0, 240.0)]
    [Nullable[double]]$TargetFps,

    [ValidateSet('Unknown', '4:4:4', '4:2:0')]
    [string]$ChromaMode = 'Unknown',

    [string]$ComputerBDisplayResolution = '',

    [Nullable[double]]$ComputerBRefreshRate,

    [string]$RemoteResolution = '',

    [string]$NetworkType = '',

    [string]$ProtectedMonitorIdentity = '',

    [string]$ExperimentMonitorIdentity = '',

    [ValidateSet('Manual', 'RemoteUiVisible')]
    [string]$RemoteUiProvenance = 'Manual',

    [string]$Notes = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RemoteProvider))
{
    throw 'RemoteProvider must explicitly name the remote-control provider'
}
$normalizedRemoteProvider = $RemoteProvider.Trim()

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

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only RemoteVisual metadata preset or partial already exists: $resolvedOutput"
}
$parent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($parent) -or -not (Test-Path -LiteralPath $parent -PathType Container))
{
    throw "RemoteVisual metadata preset parent directory does not exist: $parent"
}
$runIdBytes = [byte[]]::new(16)
$random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try
{
    $random.GetBytes($runIdBytes)
}
finally
{
    $random.Dispose()
}
$runId = ([BitConverter]::ToString($runIdBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($runIdBytes, 0, $runIdBytes.Length)
if ($runId -notmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid 128-bit lowercase hexadecimal RunId'
}

$metadata = [ordered]@{
    schema = 'PixelBridge.RemoteVisualRunMetadata.1'
    runId = $runId
    channelType = 'RemoteVisual'
    remoteProvider = $normalizedRemoteProvider
    remoteProviderVersion = $RemoteProviderVersion
    remoteMode = $RemoteMode
    targetFps = $TargetFps
    observedFps = $null
    chromaMode = $ChromaMode
    computerBDisplayResolution = $ComputerBDisplayResolution
    computerBRefreshRate = $ComputerBRefreshRate
    computerADisplayResolution = ''
    computerARefreshRate = $null
    remoteResolution = $RemoteResolution
    remoteWindowPhysicalRect = $null
    selectedRoiPhysicalRect = $null
    estimatedScaleX = $null
    estimatedScaleY = $null
    letterboxStatus = 'Unknown'
    cropStatus = 'Unknown'
    geometryStatus = 'NotObserved'
    networkType = $NetworkType
    observedBandwidthMbps = $null
    observedLatencyMilliseconds = $null
    protectedMonitorIdentity = $ProtectedMonitorIdentity
    experimentMonitorIdentity = $ExperimentMonitorIdentity
    remoteUiProvenance = $RemoteUiProvenance
    geometryProvenance = 'NotProvided'
    networkProvenance = if ([string]::IsNullOrWhiteSpace($NetworkType)) { 'NotProvided' } else { 'Manual' }
    notes = $Notes
}
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$totalMetadataStringBytes = [UInt64]0
foreach ($entry in $metadata.GetEnumerator())
{
    if ($entry.Value -isnot [string])
    {
        continue
    }
    $value = [string]$entry.Value
    if ($value.IndexOf([char]0) -ge 0)
    {
        throw "RemoteVisual metadata field contains U+0000: $($entry.Key)"
    }
    $valueBytes = [UInt64]$utf8.GetByteCount($value)
    if ($valueBytes -gt 1024)
    {
        throw "RemoteVisual metadata field exceeds 1024 UTF-8 bytes: $($entry.Key)"
    }
    $totalMetadataStringBytes += $valueBytes
}
if ($totalMetadataStringBytes -gt 8192)
{
    throw "RemoteVisual metadata string budget exceeds 8192 UTF-8 bytes: $totalMetadataStringBytes"
}
$json = $metadata | ConvertTo-Json -Depth 6
try
{
    Write-NewUtf8File -Path $temporaryOutput -Content $json
    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
}
finally
{
    if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryOutput)
    }
}
[ordered]@{
    path = $resolvedOutput
    runId = $runId
    size = [UInt64](Get-Item -LiteralPath $resolvedOutput).Length
    sha256 = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash.ToLowerInvariant()
    metadata = $metadata
} | ConvertTo-Json -Depth 8
