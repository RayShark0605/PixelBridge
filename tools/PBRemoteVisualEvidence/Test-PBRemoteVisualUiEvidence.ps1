#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$UiEvidencePath,

    [Parameter(Mandatory = $true)]
    [string]$MetadataPath,

    [Parameter(Mandatory = $true)]
    [string]$ScreenshotPath,

    [Parameter(Mandatory = $true)]
    [string]$CaptureRecordPath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [string]$OutputPath
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
        [Parameter(Mandatory = $true)][UInt64]$MaximumBytes
    )
    return Read-PBBoundedJson -Path $Path -MaximumBytes $MaximumBytes
}

function Get-FileIdentity
{
    param([Parameter(Mandatory = $true)][string]$Path)
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = [System.IO.Path]::GetFullPath($Path)
        size = [UInt64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Assert-Identity
{
    param(
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ([string]$Expected.path -cne [string]$Actual.path -or
        [UInt64]$Expected.size -ne [UInt64]$Actual.size -or
        [string]$Expected.sha256 -cne [string]$Actual.sha256)
    {
        throw "$Name exact identity mismatch"
    }
}

function Require-Rect
{
    param([Parameter(Mandatory = $true)][object]$Rect)
    foreach ($name in @('left', 'top', 'right', 'bottom'))
    {
        if (-not $Rect.ContainsKey($name) -or $Rect[$name] -isnot [int] -and $Rect[$name] -isnot [long])
        {
            throw "Screenshot capture physicalRect.$name must be an integer"
        }
    }
    $width = [Int64]$Rect.right - [Int64]$Rect.left
    $height = [Int64]$Rect.bottom - [Int64]$Rect.top
    if ($width -le 0 -or $height -le 0 -or $width -gt 8192 -or $height -gt 8192 -or $width * $height -gt 8MB)
    {
        throw 'Screenshot capture physicalRect exceeds the bounded image contract'
    }
    return [ordered]@{ width = [UInt32]$width; height = [UInt32]$height }
}

$resolvedUiEvidence = [System.IO.Path]::GetFullPath($UiEvidencePath)
$resolvedMetadata = [System.IO.Path]::GetFullPath($MetadataPath)
$resolvedScreenshot = [System.IO.Path]::GetFullPath($ScreenshotPath)
$resolvedCaptureRecord = [System.IO.Path]::GetFullPath($CaptureRecordPath)
$resolvedPython = [System.IO.Path]::GetFullPath($PythonPath)
foreach ($requiredFile in @($resolvedUiEvidence, $resolvedMetadata, $resolvedScreenshot, $resolvedCaptureRecord, $resolvedPython))
{
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf))
    {
        throw "Required UI evidence artifact does not exist: $requiredFile"
    }
    $item = Get-Item -LiteralPath $requiredFile
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    {
        throw "UI evidence artifact must not be a reparse point: $requiredFile"
    }
}
$screenshotItem = Get-Item -LiteralPath $resolvedScreenshot
if ([UInt64]$screenshotItem.Length -eq 0 -or [UInt64]$screenshotItem.Length -gt 64MB)
{
    throw 'Remote UI screenshot must be a nonempty regular file no larger than 64 MiB'
}

$evidence = Read-BoundedJson -Path $resolvedUiEvidence -MaximumBytes 128KB
$metadata = Read-BoundedJson -Path $resolvedMetadata -MaximumBytes 64KB
$captureRecord = Read-BoundedJson -Path $resolvedCaptureRecord -MaximumBytes 64KB
if ([string]$evidence.schema -cne 'PixelBridge.RemoteVisualUiEvidence.1' -or
    [string]$evidence.provenance -cne 'RemoteUiVisible' -or
    [string]$evidence.captureScope -cne 'ExperimentMonitorOnly' -or
    $evidence.containsProtectedMonitorPixels -isnot [bool] -or [bool]$evidence.containsProtectedMonitorPixels)
{
    throw 'RemoteVisual UI evidence schema or provenance contract mismatch'
}
if ([string]$metadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1' -or
    [string]$metadata.runId -cnotmatch '^[0-9a-f]{32}$' -or
    [string]$metadata.channelType -cne 'RemoteVisual' -or
    [string]$metadata.remoteUiProvenance -cne 'RemoteUiVisible' -or
    [string]$evidence.runId -cne [string]$metadata.runId)
{
    throw 'RemoteVisual metadata and UI evidence RunId/provenance do not agree'
}
if ([string]$captureRecord.schema -cne 'PixelBridge.ExperimentMonitorScreenshotCapture.1' -or
    [string]$captureRecord.captureScope -cne 'ExperimentMonitorOnly' -or
    [string]$captureRecord.collectionSemantics -cne 'Exact ExperimentMonitor physical pixels; no input, focus, window, or display mutation' -or
    $captureRecord.containsProtectedMonitorPixels -isnot [bool] -or [bool]$captureRecord.containsProtectedMonitorPixels)
{
    throw 'Screenshot capture record does not prove the ExperimentMonitor-only collection contract'
}

$metadataIdentity = Get-FileIdentity -Path $resolvedMetadata
$screenshotIdentity = Get-FileIdentity -Path $resolvedScreenshot
$captureRecordIdentity = Get-FileIdentity -Path $resolvedCaptureRecord
Assert-Identity -Expected $evidence.metadata -Actual $metadataIdentity -Name 'metadata'
Assert-Identity -Expected $evidence.screenshot -Actual $screenshotIdentity -Name 'screenshot'
Assert-Identity -Expected $evidence.captureRecord -Actual $captureRecordIdentity -Name 'capture record'
Assert-Identity -Expected $captureRecord -Actual $screenshotIdentity -Name 'capture-record screenshot'

if ([string]$captureRecord.protectedMonitorDeviceName -cne [string]$metadata.protectedMonitorIdentity -or
    [string]$captureRecord.experimentMonitorDeviceName -cne [string]$metadata.experimentMonitorIdentity -or
    [string]$evidence.captureRecord.protectedMonitorDeviceName -cne [string]$captureRecord.protectedMonitorDeviceName -or
    [string]$evidence.captureRecord.experimentMonitorDeviceName -cne [string]$captureRecord.experimentMonitorDeviceName)
{
    throw 'UI evidence monitor identities do not match the capture record and metadata'
}
foreach ($field in @('left', 'top', 'right', 'bottom'))
{
    if ([Int64]$evidence.captureRecord.physicalRect[$field] -ne [Int64]$captureRecord.physicalRect[$field])
    {
        throw 'UI evidence physical ExperimentMonitor rectangle differs from the capture record'
    }
}
$dimensions = Require-Rect -Rect $captureRecord.physicalRect
if ([UInt32]$evidence.screenshot.imageWidth -ne $dimensions.width -or
    [UInt32]$evidence.screenshot.imageHeight -ne $dimensions.height)
{
    throw 'UI evidence image dimensions differ from the physical ExperimentMonitor rectangle'
}

$claims = $evidence.visibleClaims
if ([string]$claims.remoteProvider -cne [string]$metadata.remoteProvider -or
    [string]$claims.remoteProviderVersion -cne [string]$metadata.remoteProviderVersion -or
    [string]$claims.remoteMode -cne [string]$metadata.remoteMode -or
    [string]$claims.chromaMode -cne [string]$metadata.chromaMode -or
    [string]$claims.remoteResolution -cne [string]$metadata.remoteResolution)
{
    throw 'Remote UI visible claims conflict with shared metadata'
}
if ([string]::IsNullOrWhiteSpace([string]$claims.remoteProvider) -or [string]::IsNullOrWhiteSpace([string]$claims.remoteMode))
{
    throw 'Remote UI evidence must name a visible provider and visible mode'
}
if (($null -eq $claims.targetFps) -ne ($null -eq $metadata.targetFps) -or
    ($null -ne $claims.targetFps -and ([double]::IsNaN([double]$claims.targetFps) -or
        [double]::IsInfinity([double]$claims.targetFps) -or [double]$claims.targetFps -lt 1.0 -or
        [double]$claims.targetFps -gt 240.0 -or
        [Math]::Abs([double]$claims.targetFps - [double]$metadata.targetFps) -gt 0.000001)))
{
    throw 'Remote UI visible FPS conflicts with shared metadata'
}
$expectedFields = [Collections.Generic.List[string]]::new()
[void]$expectedFields.Add('remoteProvider')
[void]$expectedFields.Add('remoteMode')
if (-not [string]::IsNullOrEmpty([string]$claims.remoteProviderVersion)) { [void]$expectedFields.Add('remoteProviderVersion') }
if (-not [string]::IsNullOrEmpty([string]$claims.remoteResolution)) { [void]$expectedFields.Add('remoteResolution') }
if ($null -ne $claims.targetFps) { [void]$expectedFields.Add('targetFps') }
if ([string]$claims.chromaMode -cne 'Unknown') { [void]$expectedFields.Add('chromaMode') }
if ($evidence.visibleFields -isnot [array] -or $evidence.visibleFields.Count -ne $expectedFields.Count)
{
    throw 'Remote UI visibleFields does not match the visible claim set'
}
for ($index = 0; $index -lt $expectedFields.Count; $index++)
{
    if ([string]$evidence.visibleFields[$index] -cne $expectedFields[$index])
    {
        throw 'Remote UI visibleFields order/content mismatch'
    }
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
    [string]$analysis.sha256 -ine [string]$screenshotIdentity.sha256 -or
    [UInt64]$analysis.sourceBytes -ne [UInt64]$screenshotIdentity.size -or
    [UInt32]$analysis.image.width -ne $dimensions.width -or [UInt32]$analysis.image.height -ne $dimensions.height -or
    [string]$evidence.screenshot.boundedAnalyzerSchema -cne [string]$analysis.schema -or
    [string]$evidence.screenshot.interpretationBoundary -cne [string]$analysis.interpretationBoundary)
{
    throw 'Bounded screenshot analyzer disagrees with sealed UI evidence'
}

$verification = [ordered]@{
    schema = 'PixelBridge.RemoteVisualUiEvidenceVerification.1'
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PASS'
    runId = [string]$metadata.runId
    provenance = 'RemoteUiVisible'
    captureScope = 'ExperimentMonitorOnly'
    containsProtectedMonitorPixels = $false
    protectedMonitorDeviceName = [string]$captureRecord.protectedMonitorDeviceName
    experimentMonitorDeviceName = [string]$captureRecord.experimentMonitorDeviceName
    visibleFields = @($expectedFields)
    uiEvidence = Get-FileIdentity -Path $resolvedUiEvidence
    metadata = $metadataIdentity
    screenshot = $screenshotIdentity
    captureRecord = $captureRecordIdentity
}
if (-not [string]::IsNullOrWhiteSpace($OutputPath))
{
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
    $temporaryOutput = "$resolvedOutput.partial"
    if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
    {
        throw "Create-only UI evidence verification output or partial already exists: $resolvedOutput"
    }
    $parent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
    if ([string]::IsNullOrWhiteSpace($parent) -or -not (Test-Path -LiteralPath $parent -PathType Container))
    {
        throw "UI evidence verification parent directory does not exist: $parent"
    }
    try
    {
        Write-NewUtf8File -Path $temporaryOutput -Content ($verification | ConvertTo-Json -Depth 12)
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
$verification | ConvertTo-Json -Depth 12
