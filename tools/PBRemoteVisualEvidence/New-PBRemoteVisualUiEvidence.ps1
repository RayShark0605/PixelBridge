#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MetadataPath,

    [Parameter(Mandatory = $true)]
    [string]$ScreenshotPath,

    [Parameter(Mandatory = $true)]
    [string]$CaptureRecordPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 128)]
    [string]$VisibleProvider,

    [string]$VisibleProviderVersion = '',

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 256)]
    [string]$VisibleMode,

    [string]$VisibleRemoteResolution = '',

    [Nullable[double]]$VisibleTargetFps,

    [ValidateSet('Unknown', '4:4:4', '4:2:0')]
    [string]$VisibleChromaMode = 'Unknown',

    [string]$Notes = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop

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

function Read-BoundedJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [UInt64]$MaximumBytes
    )
    return Read-PBBoundedJson -Path $Path -MaximumBytes $MaximumBytes
}

$resolvedMetadata = [System.IO.Path]::GetFullPath($MetadataPath)
$resolvedScreenshot = [System.IO.Path]::GetFullPath($ScreenshotPath)
$resolvedCaptureRecord = [System.IO.Path]::GetFullPath($CaptureRecordPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
$resolvedPython = [System.IO.Path]::GetFullPath($PythonPath)
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only RemoteVisual UI evidence output or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "RemoteVisual UI evidence parent directory does not exist: $outputParent"
}
foreach ($requiredFile in @($resolvedMetadata, $resolvedScreenshot, $resolvedCaptureRecord, $resolvedPython))
{
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf))
    {
        throw "Required UI evidence input does not exist: $requiredFile"
    }
    $requiredItem = Get-Item -LiteralPath $requiredFile
    if ($requiredItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    {
        throw "UI evidence input must not be a reparse point: $requiredFile"
    }
}
$screenshotItem = Get-Item -LiteralPath $resolvedScreenshot
if ([UInt64]$screenshotItem.Length -eq 0 -or [UInt64]$screenshotItem.Length -gt 64MB)
{
    throw 'Remote UI screenshot must be a nonempty regular file no larger than 64 MiB'
}

$metadata = Read-BoundedJson -Path $resolvedMetadata -MaximumBytes 64KB
if ($metadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1' -or
    [string]$metadata.runId -cnotmatch '^[0-9a-f]{32}$' -or
    [string]$metadata.channelType -cne 'RemoteVisual' -or
    [string]$metadata.remoteUiProvenance -cne 'RemoteUiVisible')
{
    throw 'RemoteVisual metadata must be a RemoteUiVisible preset with a valid RunId'
}
$captureRecord = Read-BoundedJson -Path $resolvedCaptureRecord -MaximumBytes 64KB
if ($captureRecord.schema -cne 'PixelBridge.ExperimentMonitorScreenshotCapture.1' -or
    [string]$captureRecord.captureScope -cne 'ExperimentMonitorOnly' -or
    $captureRecord.containsProtectedMonitorPixels -isnot [bool] -or
    [bool]$captureRecord.containsProtectedMonitorPixels -or
    [string]$captureRecord.collectionSemantics -cne 'Exact ExperimentMonitor physical pixels; no input, focus, window, or display mutation')
{
    throw 'Screenshot capture record does not prove the exact ExperimentMonitor-only collection contract'
}
$recordedScreenshot = [System.IO.Path]::GetFullPath([string]$captureRecord.path)
$screenshotSha256 = (Get-FileHash -LiteralPath $resolvedScreenshot -Algorithm SHA256).Hash.ToLowerInvariant()
if ($recordedScreenshot -cne $resolvedScreenshot -or
    [UInt64]$captureRecord.size -ne [UInt64]$screenshotItem.Length -or
    [string]$captureRecord.sha256 -cne $screenshotSha256)
{
    throw 'Screenshot capture record identity differs from the supplied screenshot'
}
if ((-not [string]::IsNullOrWhiteSpace([string]$metadata.protectedMonitorIdentity) -and
        [string]$captureRecord.protectedMonitorDeviceName -cne [string]$metadata.protectedMonitorIdentity) -or
    (-not [string]::IsNullOrWhiteSpace([string]$metadata.experimentMonitorIdentity) -and
        [string]$captureRecord.experimentMonitorDeviceName -cne [string]$metadata.experimentMonitorIdentity))
{
    throw 'Screenshot capture monitors conflict with the shared metadata preset'
}
if ([string]$metadata.remoteProvider -cne $VisibleProvider -or
    [string]$metadata.remoteProviderVersion -cne $VisibleProviderVersion -or
    [string]$metadata.remoteMode -cne $VisibleMode -or
    [string]$metadata.chromaMode -cne $VisibleChromaMode)
{
    throw 'Visible provider/version/mode/chroma claims conflict with the shared metadata preset'
}
if ([string]$metadata.remoteResolution -cne $VisibleRemoteResolution)
{
    throw 'Visible remote resolution conflicts with the shared metadata preset'
}
if (($null -eq $metadata.targetFps) -ne ($null -eq $VisibleTargetFps) -or
    ($null -ne $VisibleTargetFps -and ([double]::IsNaN([double]$VisibleTargetFps) -or
        [double]::IsInfinity([double]$VisibleTargetFps) -or [double]$VisibleTargetFps -lt 1.0 -or
        [double]$VisibleTargetFps -gt 240.0 -or
        [Math]::Abs([double]$metadata.targetFps - [double]$VisibleTargetFps) -gt 0.000001)))
{
    throw 'Visible target FPS conflicts with the shared metadata preset'
}

$analyzerPath = Join-Path $PSScriptRoot 'analyze_remote_capture.py'
if (-not (Test-Path -LiteralPath $analyzerPath -PathType Leaf))
{
    throw "Required bounded screenshot analyzer is missing: $analyzerPath"
}
$analysisText = @(& $resolvedPython -B $analyzerPath $resolvedScreenshot 2>&1)
if ($LASTEXITCODE -ne 0)
{
    throw "Bounded screenshot analyzer failed: $($analysisText -join [Environment]::NewLine)"
}
$analysis = ConvertFrom-PBStrictJsonText -Text ($analysisText -join "`n") -Name 'bounded screenshot analyzer output'
if ([string]$analysis.schema -cne 'PixelBridge.RemoteVisualCaptureProbe.1' -or
    [string]$analysis.sha256 -ine $screenshotSha256 -or [UInt64]$analysis.sourceBytes -ne [UInt64]$screenshotItem.Length -or
    [UInt32]$analysis.image.width -ne [UInt32]([Int64]$captureRecord.physicalRect.right - [Int64]$captureRecord.physicalRect.left) -or
    [UInt32]$analysis.image.height -ne [UInt32]([Int64]$captureRecord.physicalRect.bottom - [Int64]$captureRecord.physicalRect.top))
{
    throw 'Screenshot analyzer identity differs from the file sealed by the UI evidence tool'
}

$visibleFields = [Collections.Generic.List[string]]::new()
[void]$visibleFields.Add('remoteProvider')
[void]$visibleFields.Add('remoteMode')
if (-not [string]::IsNullOrEmpty($VisibleProviderVersion)) { [void]$visibleFields.Add('remoteProviderVersion') }
if (-not [string]::IsNullOrEmpty($VisibleRemoteResolution)) { [void]$visibleFields.Add('remoteResolution') }
if ($null -ne $VisibleTargetFps) { [void]$visibleFields.Add('targetFps') }
if ($VisibleChromaMode -cne 'Unknown') { [void]$visibleFields.Add('chromaMode') }

$evidence = [ordered]@{
    schema = 'PixelBridge.RemoteVisualUiEvidence.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    runId = [string]$metadata.runId
    captureScope = 'ExperimentMonitorOnly'
    containsProtectedMonitorPixels = $false
    provenance = 'RemoteUiVisible'
    visibleFields = @($visibleFields)
    visibleClaims = [ordered]@{
        remoteProvider = $VisibleProvider
        remoteProviderVersion = $VisibleProviderVersion
        remoteMode = $VisibleMode
        remoteResolution = $VisibleRemoteResolution
        targetFps = $VisibleTargetFps
        chromaMode = $VisibleChromaMode
    }
    metadata = [ordered]@{
        path = $resolvedMetadata
        size = [UInt64](Get-Item -LiteralPath $resolvedMetadata).Length
        sha256 = (Get-FileHash -LiteralPath $resolvedMetadata -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    captureRecord = [ordered]@{
        path = $resolvedCaptureRecord
        size = [UInt64](Get-Item -LiteralPath $resolvedCaptureRecord).Length
        sha256 = (Get-FileHash -LiteralPath $resolvedCaptureRecord -Algorithm SHA256).Hash.ToLowerInvariant()
        protectedMonitorDeviceName = [string]$captureRecord.protectedMonitorDeviceName
        experimentMonitorDeviceName = [string]$captureRecord.experimentMonitorDeviceName
        physicalRect = $captureRecord.physicalRect
    }
    screenshot = [ordered]@{
        path = $resolvedScreenshot
        size = [UInt64]$screenshotItem.Length
        sha256 = $screenshotSha256
        imageWidth = [UInt32]$analysis.image.width
        imageHeight = [UInt32]$analysis.image.height
        boundedAnalyzerSchema = [string]$analysis.schema
        interpretationBoundary = [string]$analysis.interpretationBoundary
    }
    notes = $Notes
}
$json = $evidence | ConvertTo-Json -Depth 12
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
    runId = [string]$metadata.runId
    size = [UInt64](Get-Item -LiteralPath $resolvedOutput).Length
    sha256 = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash.ToLowerInvariant()
    visibleFields = @($visibleFields)
} | ConvertTo-Json -Depth 8
