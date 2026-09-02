#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PixelBridgeDecoderPath,

    [Parameter(Mandatory = $true)]
    [string]$ProtectedMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$ExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop

$decoderPath = [System.IO.Path]::GetFullPath($PixelBridgeDecoderPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
if (-not (Test-Path -LiteralPath $decoderPath -PathType Leaf))
{
    throw "PixelBridge Decoder monitor probe does not exist: $decoderPath"
}
if ([System.IO.Path]::GetExtension($resolvedOutput) -ine '.png')
{
    throw 'Experiment-monitor screenshot output must use the .png extension'
}
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only experiment-monitor screenshot or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Experiment-monitor screenshot parent directory does not exist: $outputParent"
}
$parentItem = Get-Item -LiteralPath $outputParent
if ($parentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Experiment-monitor screenshot parent must not be a reparse point'
}
if ([string]::IsNullOrWhiteSpace($ProtectedMonitorDeviceName) -or
    [string]::IsNullOrWhiteSpace($ExperimentMonitorDeviceName) -or
    $ProtectedMonitorDeviceName.Equals($ExperimentMonitorDeviceName, [StringComparison]::OrdinalIgnoreCase))
{
    throw 'ProtectedMonitor and ExperimentMonitor must be explicit and different'
}

$monitorText = @(& $decoderPath --list-monitors 2>&1)
if ($LASTEXITCODE -ne 0)
{
    throw "PixelBridge monitor catalog failed: $($monitorText -join [Environment]::NewLine)"
}
$monitorCatalog = ConvertFrom-PBStrictJsonText -Text ($monitorText -join "`n") -Name 'PixelBridge monitor catalog output'
if ($monitorCatalog.schema -cne 'PixelBridge.MonitorCatalog.1')
{
    throw 'PixelBridge monitor catalog schema mismatch'
}
$protectedMatches = @($monitorCatalog.monitors | Where-Object {
    [string]$_.deviceName -ieq $ProtectedMonitorDeviceName
})
$experimentMatches = @($monitorCatalog.monitors | Where-Object {
    [string]$_.deviceName -ieq $ExperimentMonitorDeviceName
})
if ($protectedMatches.Count -ne 1 -or $experimentMatches.Count -ne 1)
{
    throw 'ProtectedMonitor or ExperimentMonitor is not unique in the live physical monitor catalog'
}
$protected = $protectedMatches[0].physicalRect
$experiment = $experimentMatches[0].physicalRect
$width = [Int64]$experiment.right - [Int64]$experiment.left
$height = [Int64]$experiment.bottom - [Int64]$experiment.top
if ($width -le 0 -or $height -le 0 -or $width -gt 8192 -or $height -gt 8192 -or
    $width * $height -gt 8MB)
{
    throw 'ExperimentMonitor geometry exceeds the bounded screenshot contract'
}
$monitorsOverlap = [Int64]$experiment.left -lt [Int64]$protected.right -and
    [Int64]$experiment.right -gt [Int64]$protected.left -and
    [Int64]$experiment.top -lt [Int64]$protected.bottom -and
    [Int64]$experiment.bottom -gt [Int64]$protected.top
if ($monitorsOverlap)
{
    throw 'ExperimentMonitor overlaps ProtectedMonitor; screenshot capture is forbidden'
}

Add-Type -AssemblyName System.Drawing.Common
if (-not ('PBRemoteVisualScreenshotNative' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PBRemoteVisualScreenshotNative
{
    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);
}
'@
}

$bitmap = $null
$graphics = $null
$stream = $null
$previousDpiContext = [IntPtr]::Zero
try
{
    $previousDpiContext = [PBRemoteVisualScreenshotNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
    $bitmap = [System.Drawing.Bitmap]::new([int]$width, [int]$height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen([int]$experiment.left, [int]$experiment.top, 0, 0,
        [System.Drawing.Size]::new([int]$width, [int]$height), [System.Drawing.CopyPixelOperation]::SourceCopy)
    $stream = [System.IO.File]::Open($temporaryOutput, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $stream.Flush($true)
    $stream.Dispose()
    $stream = $null
    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
}
finally
{
    if ($null -ne $stream) { $stream.Dispose() }
    if ($null -ne $graphics) { $graphics.Dispose() }
    if ($null -ne $bitmap) { $bitmap.Dispose() }
    if ($previousDpiContext -ne [IntPtr]::Zero)
    {
        [void][PBRemoteVisualScreenshotNative]::SetThreadDpiAwarenessContext($previousDpiContext)
    }
    if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryOutput)
    }
}

$outputItem = Get-Item -LiteralPath $resolvedOutput
[ordered]@{
    schema = 'PixelBridge.ExperimentMonitorScreenshotCapture.1'
    capturedUtc = [DateTime]::UtcNow.ToString('o')
    collectionSemantics = 'Exact ExperimentMonitor physical pixels; no input, focus, window, or display mutation'
    captureScope = 'ExperimentMonitorOnly'
    containsProtectedMonitorPixels = $false
    protectedMonitorDeviceName = [string]$protectedMatches[0].deviceName
    experimentMonitorDeviceName = [string]$experimentMatches[0].deviceName
    physicalRect = [ordered]@{
        left = [int]$experiment.left
        top = [int]$experiment.top
        right = [int]$experiment.right
        bottom = [int]$experiment.bottom
    }
    path = $resolvedOutput
    size = [UInt64]$outputItem.Length
    sha256 = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json -Depth 8
