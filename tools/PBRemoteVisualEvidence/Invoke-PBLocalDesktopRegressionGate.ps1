#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EncoderPath,

    [Parameter(Mandatory = $true)]
    [string]$DecoderPath,

    [Parameter(Mandatory = $true)]
    [string]$PackageManifestPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPackageManifestSha256,

    [Parameter(Mandatory = $true)]
    [string]$BaselineSummaryPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedBaselineSummarySha256,

    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [ValidateSet('None', 'Possible', 'Present')]
    [string]$ProtectedMonitorConcurrentWork = 'Possible',

    [switch]$PreflightOnly,

    [ValidateRange(100, 2000)]
    [UInt32]$DecoderWarmupMilliseconds = 250,

    [UInt32]$EncoderSeconds = 30,

    [UInt32]$DecoderTimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Security.Cryptography
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class PbLocalDesktopRegressionWindowProbe
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public sealed class WindowInfo
    {
        public long Handle;
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
        public bool Visible;
    }

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr data);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr data);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    public static uint ForegroundProcessId()
    {
        uint processId;
        GetWindowThreadProcessId(GetForegroundWindow(), out processId);
        return processId;
    }

    public static WindowInfo[] WindowsForProcess(uint targetProcessId)
    {
        var output = new List<WindowInfo>();
        EnumWindowsProc callback = delegate(IntPtr window, IntPtr data)
        {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == targetProcessId)
            {
                RECT rect;
                if (GetWindowRect(window, out rect))
                {
                    output.Add(new WindowInfo
                    {
                        Handle = window.ToInt64(),
                        Left = rect.Left,
                        Top = rect.Top,
                        Right = rect.Right,
                        Bottom = rect.Bottom,
                        Visible = IsWindowVisible(window)
                    });
                }
            }
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        return output.ToArray();
    }
}
"@

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try
    {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
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

function Write-NewJsonFile
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    Write-NewUtf8File -Path $Path -Content (($Value | ConvertTo-Json -Depth 40) + [Environment]::NewLine)
}

function Get-FileIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$RelativeTo
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $resolvedPath
    if (-not $item.PSIsContainer -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Reparse-point evidence file is not permitted: $resolvedPath"
    }
    $reportedPath = if ([string]::IsNullOrEmpty($RelativeTo))
    {
        $resolvedPath
    }
    else
    {
        [IO.Path]::GetRelativePath($RelativeTo, $resolvedPath).Replace('\', '/')
    }
    return [ordered]@{
        path = $reportedPath
        size = [Int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function New-CsprngRunId
{
    $bytes = [byte[]]::new(16)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return ([Convert]::ToHexString($bytes)).ToLowerInvariant()
}

function Require-Finite
{
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$Positive,
        [switch]$UnitInterval
    )

    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or
        ($Positive -and $number -le 0) -or ($UnitInterval -and ($number -lt 0 -or $number -gt 1)))
    {
        throw "$Name is outside its finite numeric contract"
    }
    return $number
}

function Get-GpuSample
{
    param([int[]]$ProcessIds)

    try
    {
        $counter = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -MaxSamples 1 -ErrorAction Stop
        $values = [ordered]@{}
        foreach ($processId in $ProcessIds)
        {
            $samples = @($counter.CounterSamples | Where-Object {
                    $_.InstanceName -match ("^pid_{0}_" -f $processId)
                } | ForEach-Object { [double]$_.CookedValue })
            $values[[string]$processId] = if ($samples.Count -eq 0)
            {
                0.0
            }
            else
            {
                [double](($samples | Measure-Object -Sum).Sum)
            }
        }
        return [ordered]@{ available = $true; values = $values; reason = $null }
    }
    catch
    {
        return [ordered]@{ available = $false; values = [ordered]@{}; reason = $_.Exception.Message }
    }
}

function Get-ProcessResourceSample
{
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$EncoderProcess,
        [Parameter(Mandatory = $true)][Diagnostics.Process]$DecoderProcess,
        [Parameter(Mandatory = $true)][Diagnostics.Stopwatch]$Stopwatch
    )

    $EncoderProcess.Refresh()
    $DecoderProcess.Refresh()
    $activeProcessIds = [Collections.Generic.List[int]]::new()
    if (-not $EncoderProcess.HasExited)
    {
        $activeProcessIds.Add($EncoderProcess.Id)
    }
    if (-not $DecoderProcess.HasExited)
    {
        $activeProcessIds.Add($DecoderProcess.Id)
    }
    $gpu = Get-GpuSample -ProcessIds @($activeProcessIds)
    $EncoderProcess.Refresh()
    $DecoderProcess.Refresh()
    return [ordered]@{
        utc = [DateTime]::UtcNow.ToString('O')
        elapsedMilliseconds = [Int64]$Stopwatch.ElapsedMilliseconds
        foregroundProcessId = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
        encoder = [ordered]@{
            processId = $EncoderProcess.Id
            exited = $EncoderProcess.HasExited
            cpuMilliseconds = [double]$EncoderProcess.TotalProcessorTime.TotalMilliseconds
            privateBytes = [Int64]$EncoderProcess.PrivateMemorySize64
            workingSetBytes = [Int64]$EncoderProcess.WorkingSet64
            handles = [Int64]$EncoderProcess.HandleCount
        }
        decoder = [ordered]@{
            processId = $DecoderProcess.Id
            exited = $DecoderProcess.HasExited
            cpuMilliseconds = [double]$DecoderProcess.TotalProcessorTime.TotalMilliseconds
            privateBytes = [Int64]$DecoderProcess.PrivateMemorySize64
            workingSetBytes = [Int64]$DecoderProcess.WorkingSet64
            handles = [Int64]$DecoderProcess.HandleCount
        }
        gpu = $gpu
    }
}

function Get-ProcessGpuSummary
{
    param(
        [Parameter(Mandatory = $true)][object[]]$Samples,
        [Parameter(Mandatory = $true)][int]$ProcessId
    )

    $values = [Collections.Generic.List[double]]::new()
    $unavailableReasons = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($sample in $Samples)
    {
        if (-not [bool]$sample.gpu.available)
        {
            if (-not [string]::IsNullOrEmpty([string]$sample.gpu.reason))
            {
                [void]$unavailableReasons.Add([string]$sample.gpu.reason)
            }
            continue
        }
        $key = [string]$ProcessId
        $valuePresent = $false
        $sampleValue = $null
        if ($sample.gpu.values -is [Collections.IDictionary])
        {
            $valuePresent = $sample.gpu.values.Contains($key)
            if ($valuePresent)
            {
                $sampleValue = $sample.gpu.values[$key]
            }
        }
        else
        {
            $property = $sample.gpu.values.PSObject.Properties[$key]
            $valuePresent = $null -ne $property
            if ($valuePresent)
            {
                $sampleValue = $property.Value
            }
        }
        if ($valuePresent)
        {
            $value = Require-Finite -Value $sampleValue -Name "GPU sample for PID $ProcessId"
            if ($value -lt 0)
            {
                throw "GPU sample for PID $ProcessId is negative"
            }
            $values.Add($value)
        }
    }
    return [ordered]@{
        available = $values.Count -gt 0
        sampleCount = $values.Count
        unavailableSampleCount = $Samples.Count - $values.Count
        averagePercent = if ($values.Count -eq 0) { $null } else { [double](($values | Measure-Object -Average).Average) }
        peakPercent = if ($values.Count -eq 0) { $null } else { [double](($values | Measure-Object -Maximum).Maximum) }
        unavailableReasons = @($unavailableReasons | Sort-Object)
    }
}

function Get-ResourceHighWater
{
    param(
        [Parameter(Mandatory = $true)][object[]]$Samples,
        [Parameter(Mandatory = $true)][ValidateSet('encoder', 'decoder')][string]$Role
    )

    if ($Samples.Count -lt 2)
    {
        throw "At least two resource samples are required for $Role"
    }
    return [ordered]@{
        privateBytes = [Int64](($Samples | ForEach-Object { $_.$Role.privateBytes } | Measure-Object -Maximum).Maximum)
        workingSetBytes = [Int64](($Samples | ForEach-Object { $_.$Role.workingSetBytes } | Measure-Object -Maximum).Maximum)
        handles = [Int64](($Samples | ForEach-Object { $_.$Role.handles } | Measure-Object -Maximum).Maximum)
    }
}

function Test-WindowContainment
{
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][object]$ExperimentMonitor,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][Collections.Generic.List[object]]$Samples
    )

    $Process.Refresh()
    if ($Process.HasExited)
    {
        return $true
    }
    $inside = $true
    foreach ($window in [PbLocalDesktopRegressionWindowProbe]::WindowsForProcess([UInt32]$Process.Id))
    {
        if (-not $window.Visible -or $window.Right -le $window.Left -or $window.Bottom -le $window.Top)
        {
            continue
        }
        $currentInside = $window.Left -ge [int]$ExperimentMonitor.physicalRect.left -and
            $window.Top -ge [int]$ExperimentMonitor.physicalRect.top -and
            $window.Right -le [int]$ExperimentMonitor.physicalRect.right -and
            $window.Bottom -le [int]$ExperimentMonitor.physicalRect.bottom
        $Samples.Add([ordered]@{
                utc = [DateTime]::UtcNow.ToString('O')
                processId = $Process.Id
                handle = ('0x{0:x}' -f $window.Handle)
                left = $window.Left
                top = $window.Top
                right = $window.Right
                bottom = $window.Bottom
                insideExperimentMonitor = $currentInside
            })
        if (-not $currentInside)
        {
            $inside = $false
        }
    }
    return $inside
}

function Get-BaselineGpuSummary
{
    param(
        [Parameter(Mandatory = $true)][string]$BaselineRoot,
        [Parameter(Mandatory = $true)][string]$CaseName
    )

    $caseRoot = Join-Path $BaselineRoot (Join-Path 'localdesktop-reference' $CaseName)
    $resourcePath = Join-Path $caseRoot 'resource-samples.json'
    $windowPath = Join-Path $caseRoot 'window-samples.json'
    $samples = @(Get-Content -Raw -LiteralPath $resourcePath | ConvertFrom-Json)
    $windows = @(Get-Content -Raw -LiteralPath $windowPath | ConvertFrom-Json)
    $encoderProcessIds = @($windows | ForEach-Object { [int]$_.pid } | Sort-Object -Unique)
    if ($encoderProcessIds.Count -ne 1 -or $samples.Count -eq 0)
    {
        throw "Baseline process identity is ambiguous for $CaseName"
    }
    $firstAvailable = $samples | Where-Object { [bool]$_.gpu.available } | Select-Object -First 1
    if ($null -eq $firstAvailable)
    {
        return [ordered]@{
            encoder = [ordered]@{ available = $false; sampleCount = 0; unavailableSampleCount = $samples.Count; averagePercent = $null; peakPercent = $null; unavailableReasons = @('No baseline GPU sample was available') }
            decoder = [ordered]@{ available = $false; sampleCount = 0; unavailableSampleCount = $samples.Count; averagePercent = $null; peakPercent = $null; unavailableReasons = @('No baseline GPU sample was available') }
            resourceSamples = Get-FileIdentity -Path $resourcePath
            windowSamples = Get-FileIdentity -Path $windowPath
        }
    }
    $processIds = @($firstAvailable.gpu.values.PSObject.Properties.Name | ForEach-Object { [int]$_ })
    if ($processIds.Count -ne 2 -or $encoderProcessIds[0] -notin $processIds)
    {
        throw "Baseline GPU process mapping is ambiguous for $CaseName"
    }
    $decoderProcessId = @($processIds | Where-Object { $_ -ne $encoderProcessIds[0] })[0]
    return [ordered]@{
        encoder = Get-ProcessGpuSummary -Samples $samples -ProcessId $encoderProcessIds[0]
        decoder = Get-ProcessGpuSummary -Samples $samples -ProcessId $decoderProcessId
        resourceSamples = Get-FileIdentity -Path $resourcePath
        windowSamples = Get-FileIdentity -Path $windowPath
    }
}

function Get-CaseArtifactInventory
{
    param(
        [Parameter(Mandatory = $true)][string]$CaseRoot,
        [Parameter(Mandatory = $true)][string]$EvidenceRoot
    )

    return @(Get-ChildItem -LiteralPath $CaseRoot -Recurse -File -Force | Where-Object {
            $_.Name -cne 'case-summary.json'
        } | Sort-Object { [IO.Path]::GetRelativePath($EvidenceRoot, $_.FullName).Replace('\', '/') } | ForEach-Object {
            Get-FileIdentity -Path $_.FullName -RelativeTo $EvidenceRoot
        })
}

function Get-DecoderJournalAnalysis
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RunId
    )

    $records = [Collections.Generic.List[object]]::new()
    $lastUnixMilliseconds = [Int64]::MinValue
    $lastResourceRejections = [UInt64]0
    foreach ($line in @(Get-Content -LiteralPath $Path))
    {
        if ([string]::IsNullOrWhiteSpace($line))
        {
            throw 'Decoder journal contains an empty record'
        }
        $record = $line | ConvertFrom-Json
        if ($record.schema -cne 'PixelBridge.RunJournal.1' -or $record.role -cne 'Decoder' -or
            $record.runId -cne $RunId -or [Int64]$record.unixMs -lt $lastUnixMilliseconds -or
            [UInt64]$record.outerResourceRejections -lt $lastResourceRejections -or
            [UInt64]$record.outerConflictRejections -ne 0)
        {
            throw 'Decoder journal identity, ordering, or conflict contract failed'
        }
        $lastUnixMilliseconds = [Int64]$record.unixMs
        $lastResourceRejections = [UInt64]$record.outerResourceRejections
        $records.Add($record)
    }
    if ($records.Count -lt 2)
    {
        throw 'Decoder journal did not contain periodic and terminal records'
    }
    $descriptorRecords = @($records | Where-Object { [bool]$_.descriptorKnown })
    $terminal = $records[$records.Count - 1]
    if ($descriptorRecords.Count -eq 0 -or $terminal.state -cne 'Completed' -or
        -not [bool]$terminal.wholeFileDigestPass -or -not [bool]$terminal.finalPublishPass)
    {
        throw 'Decoder journal never reached authoritative descriptor/completion state'
    }
    $firstDescriptorRecord = $descriptorRecords[0]
    $postDescriptorResourceRejectionIncrease = [UInt64]$terminal.outerResourceRejections -
        [UInt64]$firstDescriptorRecord.outerResourceRejections
    if ($postDescriptorResourceRejectionIncrease -ne 0)
    {
        throw 'Receiver resource rejections increased after the first sampled descriptor-bound state'
    }
    return [ordered]@{
        recordCount = $records.Count
        preDescriptorRecordCount = @($records | Where-Object { -not [bool]$_.descriptorKnown }).Count
        descriptorKnownRecordCount = $descriptorRecords.Count
        firstDescriptorKnownUnixMilliseconds = [Int64]$firstDescriptorRecord.unixMs
        resourceRejectionsAtFirstDescriptorSample = [UInt64]$firstDescriptorRecord.outerResourceRejections
        finalResourceRejections = [UInt64]$terminal.outerResourceRejections
        postDescriptorResourceRejectionIncrease = $postDescriptorResourceRejectionIncrease
        finalConflictRejections = [UInt64]$terminal.outerConflictRejections
        terminalState = [string]$terminal.state
        terminalWholeFileDigestPass = [bool]$terminal.wholeFileDigestPass
        terminalFinalPublishPass = [bool]$terminal.finalPublishPass
    }
}

$resolvedEncoderPath = (Resolve-Path -LiteralPath $EncoderPath).Path
$resolvedDecoderPath = (Resolve-Path -LiteralPath $DecoderPath).Path
$resolvedPackageManifestPath = (Resolve-Path -LiteralPath $PackageManifestPath).Path
$resolvedBaselineSummaryPath = (Resolve-Path -LiteralPath $BaselineSummaryPath).Path
$resolvedSourcePath = (Resolve-Path -LiteralPath $SourcePath).Path
$resolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)

if (Test-Path -LiteralPath $resolvedOutputRoot)
{
    throw "OutputRoot already exists: $resolvedOutputRoot"
}
if ($EncoderSeconds -lt 10 -or $EncoderSeconds -gt 600 -or $DecoderTimeoutSeconds -lt 10 -or $DecoderTimeoutSeconds -gt 600)
{
    throw 'EncoderSeconds and DecoderTimeoutSeconds must each be within 10..600'
}

$packageManifestIdentity = Get-FileIdentity -Path $resolvedPackageManifestPath
if ($packageManifestIdentity.sha256 -cne $ExpectedPackageManifestSha256)
{
    throw 'Package manifest hash does not match the expected Step 18 identity'
}
$packageManifest = Get-Content -Raw -LiteralPath $resolvedPackageManifestPath | ConvertFrom-Json
if ($packageManifest.schema -cne 'PixelBridge.PortablePackage.2')
{
    throw 'Package manifest schema is not PixelBridge.PortablePackage.2'
}
$encoderApplication = @($packageManifest.applications | Where-Object { $_.role -ceq 'Encoder' })
$decoderApplication = @($packageManifest.applications | Where-Object { $_.role -ceq 'Decoder' })
if ($encoderApplication.Count -ne 1 -or $decoderApplication.Count -ne 1)
{
    throw 'Package manifest must contain exactly one Encoder and one Decoder application'
}
$encoderIdentity = Get-FileIdentity -Path $resolvedEncoderPath
$decoderIdentity = Get-FileIdentity -Path $resolvedDecoderPath
if ($encoderIdentity.size -ne [Int64]$encoderApplication[0].size -or
    $encoderIdentity.sha256 -cne [string]$encoderApplication[0].sha256 -or
    $decoderIdentity.size -ne [Int64]$decoderApplication[0].size -or
    $decoderIdentity.sha256 -cne [string]$decoderApplication[0].sha256)
{
    throw 'Selected Encoder or Decoder does not match the Step 18 package manifest'
}

$baselineIdentity = Get-FileIdentity -Path $resolvedBaselineSummaryPath
if ($baselineIdentity.sha256 -cne $ExpectedBaselineSummarySha256)
{
    throw 'Baseline summary hash does not match the sealed pre-change identity'
}
$baseline = Get-Content -Raw -LiteralPath $resolvedBaselineSummaryPath | ConvertFrom-Json
if ($baseline.schema -cne 'PixelBridge.P1_5.PrechangeLocalDesktopReference.1' -or
    $baseline.status -cne 'PASS' -or $baseline.gitCommit -cne 'cd533feceaaf28b925e4b6878e589747ea159c67' -or
    [Int64]$baseline.sourceBytes -ne 8MB -or [string]$baseline.sourceSha256 -cne 'e02206c8813b212705ce995f49498bfacdd49030f097492a6eb2777ea66a4fe2')
{
    throw 'Baseline summary does not match the frozen Step 19 pre-change contract'
}
$sourceIdentity = Get-FileIdentity -Path $resolvedSourcePath
if ($sourceIdentity.size -ne [Int64]$baseline.sourceBytes -or $sourceIdentity.sha256 -cne [string]$baseline.sourceSha256)
{
    throw 'Selected source is not the exact frozen 8 MiB RAW baseline source'
}

$expectedCases = @('wgc-direct', 'dxgi-direct', 'wgc-shape', 'dxgi-shape')
$baselineCases = @($baseline.cases)
if ($baselineCases.Count -ne $expectedCases.Count -or
    (@($baselineCases.name | Sort-Object) -join ',') -cne (@($expectedCases | Sort-Object) -join ','))
{
    throw 'Baseline does not contain exactly the four Step 19 matrix cases'
}
foreach ($baselineCase in $baselineCases)
{
    if ([Int64]$baselineCase.sourceBytes -ne $sourceIdentity.size -or
        [string]$baselineCase.sourceSha256 -cne $sourceIdentity.sha256 -or
        -not [bool]$baselineCase.sourceOutputLengthEqual -or
        -not [bool]$baselineCase.sourceOutputSha256Equal -or
        -not [bool]$baselineCase.encoderAliveWhenDecoderCompleted -or
        [string]$baselineCase.decoderState -cne 'Completed' -or
        -not [bool]$baselineCase.wholeFileDigestVerified -or
        -not [bool]$baselineCase.finalPublishSucceeded -or
        [Int64]$baselineCase.outputBytes -ne $sourceIdentity.size -or
        [string]$baselineCase.outputSha256 -cne $sourceIdentity.sha256 -or
        [bool]$baselineCase.focusViolation -or [bool]$baselineCase.containmentViolation)
    {
        throw "Baseline authoritative completion contract failed for $($baselineCase.name)"
    }
    $baselineOutputIdentity = Get-FileIdentity -Path ([string]$baselineCase.output)
    if ($baselineOutputIdentity.size -ne $sourceIdentity.size -or $baselineOutputIdentity.sha256 -cne $sourceIdentity.sha256)
    {
        throw "Baseline external output no longer matches for $($baselineCase.name)"
    }
    [void](Require-Finite -Value $baselineCase.verifiedEncodedGoodputBitsPerSecond -Name "baseline goodput $($baselineCase.name)" -Positive)
    [void](Require-Finite -Value $baselineCase.fecFrameErrorRate -Name "baseline FER $($baselineCase.name)" -UnitInterval)
}

$monitorOutput = @(& $resolvedDecoderPath --list-monitors 2>&1)
$monitorExitCode = $LASTEXITCODE
if ($monitorExitCode -ne 0)
{
    throw "Decoder monitor enumeration failed with exit $monitorExitCode"
}
$monitorCatalog = ($monitorOutput | Out-String) | ConvertFrom-Json
if ($monitorCatalog.schema -cne 'PixelBridge.MonitorCatalog.1')
{
    throw 'Decoder monitor catalog schema mismatch'
}
$protectedMonitor = @($monitorCatalog.monitors | Where-Object { $_.deviceName -ceq [string]$baseline.protectedMonitor.deviceName })
$experimentMonitor = @($monitorCatalog.monitors | Where-Object { $_.deviceName -ceq [string]$baseline.experimentMonitor.deviceName })
if ($protectedMonitor.Count -ne 1 -or $experimentMonitor.Count -ne 1)
{
    throw 'Frozen protected or experiment monitor identity is not uniquely present'
}
$protectedMonitor = $protectedMonitor[0]
$experimentMonitor = $experimentMonitor[0]
foreach ($binding in @(
        [ordered]@{ live = $protectedMonitor; baseline = $baseline.protectedMonitor; name = 'protected' },
        [ordered]@{ live = $experimentMonitor; baseline = $baseline.experimentMonitor; name = 'experiment' }))
{
    if ([int]$binding.live.physicalRect.left -ne [int]$binding.baseline.left -or
        [int]$binding.live.physicalRect.top -ne [int]$binding.baseline.top -or
        [int]$binding.live.physicalRect.right -ne [int]$binding.baseline.right -or
        [int]$binding.live.physicalRect.bottom -ne [int]$binding.baseline.bottom -or
        [string]$binding.live.rotation -cne 'Identity')
    {
        throw "Live $($binding.name) monitor geometry differs from the frozen baseline"
    }
}
$roi = $baseline.dataWindowAndRoi
if ([int]$roi.left -lt [int]$experimentMonitor.physicalRect.left -or
    [int]$roi.top -lt [int]$experimentMonitor.physicalRect.top -or
    [int]$roi.right -gt [int]$experimentMonitor.physicalRect.right -or
    [int]$roi.bottom -gt [int]$experimentMonitor.physicalRect.bottom -or
    [int]$roi.right - [int]$roi.left -ne 1920 -or [int]$roi.bottom - [int]$roi.top -ne 1080)
{
    throw 'Frozen ROI is not the exact 1920x1080 right-monitor rectangle'
}

$activeEdidSerials = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop |
    Where-Object { [bool]$_.Active } | ForEach-Object {
        -join ($_.SerialNumberID | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
    } | Sort-Object -Unique)
if ([string]$baseline.protectedMonitor.serial -notin $activeEdidSerials -or
    [string]$baseline.experimentMonitor.serial -notin $activeEdidSerials)
{
    throw 'One or both frozen physical-monitor serials are not active'
}

[IO.Directory]::CreateDirectory($resolvedOutputRoot) | Out-Null
$preflight = [ordered]@{
    schema = 'PixelBridge.LocalDesktopRegressionPreflight.1'
    generatedUtc = [DateTime]::UtcNow.ToString('O')
    packageManifest = $packageManifestIdentity
    packageName = [string]$packageManifest.packageName
    headCommit = [string]$packageManifest.buildIdentity.headCommit
    headTree = [string]$packageManifest.buildIdentity.headTree
    testedSourceFingerprintSha256 = [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256
    buildIdentityFingerprintSha256 = [string]$packageManifest.buildIdentityFingerprintSha256
    packagePayloadFingerprintSha256 = [string]$packageManifest.packagePayloadFingerprintSha256
    encoder = $encoderIdentity
    decoder = $decoderIdentity
    baselineSummary = $baselineIdentity
    source = $sourceIdentity
    replay = [ordered]@{ enabled = $false; semantics = 'No replay CLI option is passed; each Decoder report must confirm replay.enabled=false' }
    protectedMonitorConcurrentWork = $ProtectedMonitorConcurrentWork
    monitorCatalog = $monitorCatalog
    activeEdidSerials = $activeEdidSerials
    protectedMonitorSerial = [string]$baseline.protectedMonitor.serial
    experimentMonitorSerial = [string]$baseline.experimentMonitor.serial
    dataWindowAndRoi = [ordered]@{ left = [int]$roi.left; top = [int]$roi.top; right = [int]$roi.right; bottom = [int]$roi.bottom }
    launchPolicy = [ordered]@{
        order = 'DecoderBeforeEncoder'
        decoderWarmupMilliseconds = $DecoderWarmupMilliseconds
        rationale = 'The Decoder process starts before the Encoder, removing the prior fixed three-second mid-Carousel launch bias; bounded cold-start acquisition remains inside the measured interval and is journaled.'
        verifiedEncodedGoodputWindow = 'PBTelemetry first captured observation through authoritative verified encoded-byte completion; decoder warmup therefore remains inside the measured interval.'
    }
    encoderSeconds = $EncoderSeconds
    decoderTimeoutSeconds = $DecoderTimeoutSeconds
}
Write-NewJsonFile -Path (Join-Path $resolvedOutputRoot 'preflight.json') -Value $preflight

if ($PreflightOnly)
{
    [ordered]@{
        schema = 'PixelBridge.LocalDesktopRegressionPreflightResult.1'
        status = 'PASS'
        evidenceRoot = $resolvedOutputRoot
        preflight = Get-FileIdentity -Path (Join-Path $resolvedOutputRoot 'preflight.json')
    } | ConvertTo-Json -Depth 6
    return
}

$matrixForegroundBefore = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
$postCases = [Collections.Generic.List[object]]::new()
$caseDefinitions = @(
    [ordered]@{ name = 'wgc-direct'; backend = 'wgc'; profile = 'direct' },
    [ordered]@{ name = 'dxgi-direct'; backend = 'dxgi'; profile = 'direct' },
    [ordered]@{ name = 'wgc-shape'; backend = 'wgc'; profile = 'shape' },
    [ordered]@{ name = 'dxgi-shape'; backend = 'dxgi'; profile = 'shape' }
)

foreach ($caseDefinition in $caseDefinitions)
{
    Write-Host "PB_STEP19_CASE_START name=$($caseDefinition.name)"
    $caseRoot = Join-Path $resolvedOutputRoot $caseDefinition.name
    $outputDirectory = Join-Path $caseRoot 'output'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $encoderReportPath = Join-Path $caseRoot 'encoder-report.json'
    $decoderReportPath = Join-Path $caseRoot 'decoder-report.json'
    $decoderJournalPath = Join-Path $caseRoot 'decoder-journal.jsonl'
    $encoderStdoutPath = Join-Path $caseRoot 'encoder.stdout.log'
    $encoderStderrPath = Join-Path $caseRoot 'encoder.stderr.log'
    $decoderStdoutPath = Join-Path $caseRoot 'decoder.stdout.log'
    $decoderStderrPath = Join-Path $caseRoot 'decoder.stderr.log'
    $runId = New-CsprngRunId
    $encoderArguments = '--headless-broadcast --source "{0}" --profile {1} --channel local --compression off --origin {2} {3} --seconds {4} --logical-fps 0 --control-repetitions 4 --run-id {5} --report "{6}"' -f
        $resolvedSourcePath, $caseDefinition.profile, $roi.left, $roi.top, $EncoderSeconds, $runId, $encoderReportPath
    $decoderArguments = '--headless-receive --output-dir "{0}" --backend {1} --profile {2} --channel local --roi {3} {4} {5} {6} --timeout {7} --run-id {8} --journal "{9}" --report "{10}"' -f
        $outputDirectory, $caseDefinition.backend, $caseDefinition.profile, $roi.left, $roi.top, $roi.right, $roi.bottom,
        $DecoderTimeoutSeconds, $runId, $decoderJournalPath, $decoderReportPath

    $foregroundBefore = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
    $encoderProcess = $null
    $decoderProcess = $null
    $encoderAliveWhenDecoderCompleted = $false
    $focusViolation = $false
    $containmentViolation = $false
    $windowSamples = [Collections.Generic.List[object]]::new()
    $resourceSamples = [Collections.Generic.List[object]]::new()
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    try
    {
        $decoderProcess = Start-Process -FilePath $resolvedDecoderPath -ArgumentList $decoderArguments -WorkingDirectory (Split-Path -Parent $resolvedDecoderPath) -WindowStyle Hidden -RedirectStandardOutput $decoderStdoutPath -RedirectStandardError $decoderStderrPath -PassThru
        $warmupDeadline = [DateTime]::UtcNow.AddMilliseconds($DecoderWarmupMilliseconds)
        while ([DateTime]::UtcNow -lt $warmupDeadline)
        {
            $decoderProcess.Refresh()
            if ($decoderProcess.HasExited)
            {
                throw "Decoder exited during the capture-ready warmup for $($caseDefinition.name)"
            }
            $foreground = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
            if ($foreground -eq [UInt32]$decoderProcess.Id)
            {
                $focusViolation = $true
            }
            if (-not (Test-WindowContainment -Process $decoderProcess -ExperimentMonitor $experimentMonitor -Samples $windowSamples))
            {
                $containmentViolation = $true
            }
            if ($focusViolation -or $containmentViolation)
            {
                throw "Decoder capture-ready warmup violated screen safety for $($caseDefinition.name)"
            }
            Start-Sleep -Milliseconds 25
        }

        $encoderProcess = Start-Process -FilePath $resolvedEncoderPath -ArgumentList $encoderArguments -WorkingDirectory (Split-Path -Parent $resolvedEncoderPath) -WindowStyle Hidden -RedirectStandardOutput $encoderStdoutPath -RedirectStandardError $encoderStderrPath -PassThru
        $windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
        $windowReady = $false
        while (-not $encoderProcess.HasExited -and [DateTime]::UtcNow -lt $windowDeadline)
        {
            $decoderProcess.Refresh()
            if ($decoderProcess.HasExited)
            {
                throw "Decoder exited before the Encoder Data Window became ready for $($caseDefinition.name)"
            }
            $foreground = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
            if ($foreground -eq [UInt32]$encoderProcess.Id -or $foreground -eq [UInt32]$decoderProcess.Id)
            {
                $focusViolation = $true
            }
            if (-not (Test-WindowContainment -Process $encoderProcess -ExperimentMonitor $experimentMonitor -Samples $windowSamples) -or
                -not (Test-WindowContainment -Process $decoderProcess -ExperimentMonitor $experimentMonitor -Samples $windowSamples))
            {
                $containmentViolation = $true
            }
            $visibleWindows = @([PbLocalDesktopRegressionWindowProbe]::WindowsForProcess([UInt32]$encoderProcess.Id) | Where-Object {
                    $_.Visible -and $_.Left -eq [int]$roi.left -and $_.Top -eq [int]$roi.top -and
                    $_.Right -eq [int]$roi.right -and $_.Bottom -eq [int]$roi.bottom
                })
            if ($visibleWindows.Count -eq 1)
            {
                $windowReady = $true
                break
            }
            Start-Sleep -Milliseconds 100
            $encoderProcess.Refresh()
        }
        if (-not $windowReady)
        {
            throw "Encoder Data Window did not reach the exact frozen ROI for $($caseDefinition.name)"
        }
        if ($focusViolation -or $containmentViolation)
        {
            throw "Screen safety preflight failed for $($caseDefinition.name)"
        }

        $deadline = [DateTime]::UtcNow.AddSeconds([Math]::Max($DecoderTimeoutSeconds + 15, $EncoderSeconds + 20))
        while (-not $decoderProcess.HasExited -and [DateTime]::UtcNow -lt $deadline)
        {
            $foreground = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
            if ($foreground -eq [UInt32]$encoderProcess.Id -or $foreground -eq [UInt32]$decoderProcess.Id)
            {
                $focusViolation = $true
            }
            if (-not (Test-WindowContainment -Process $encoderProcess -ExperimentMonitor $experimentMonitor -Samples $windowSamples) -or
                -not (Test-WindowContainment -Process $decoderProcess -ExperimentMonitor $experimentMonitor -Samples $windowSamples))
            {
                $containmentViolation = $true
            }
            $resourceSamples.Add((Get-ProcessResourceSample -EncoderProcess $encoderProcess -DecoderProcess $decoderProcess -Stopwatch $stopwatch))
            if ($focusViolation -or $containmentViolation)
            {
                throw "Screen safety changed while running $($caseDefinition.name)"
            }
            Start-Sleep -Milliseconds 100
            $decoderProcess.Refresh()
        }
        if (-not $decoderProcess.HasExited)
        {
            throw "Decoder deadline exceeded for $($caseDefinition.name)"
        }
        $encoderProcess.Refresh()
        $encoderAliveWhenDecoderCompleted = -not $encoderProcess.HasExited
        if (-not $encoderProcess.HasExited -and -not $encoderProcess.WaitForExit(([int]$EncoderSeconds + 20) * 1000))
        {
            throw "Encoder deadline exceeded for $($caseDefinition.name)"
        }
    }
    finally
    {
        $stopwatch.Stop()
        if ($null -ne $decoderProcess -and -not $decoderProcess.HasExited)
        {
            $decoderProcess.Kill($true)
            $decoderProcess.WaitForExit()
        }
        if ($null -ne $encoderProcess -and -not $encoderProcess.HasExited)
        {
            $encoderProcess.Kill($true)
            $encoderProcess.WaitForExit()
        }
    }

    $encoderProcess.Refresh()
    $decoderProcess.Refresh()
    if ($encoderProcess.ExitCode -ne 0 -or $decoderProcess.ExitCode -ne 0)
    {
        throw "Application failed for $($caseDefinition.name): encoder=$($encoderProcess.ExitCode) decoder=$($decoderProcess.ExitCode)"
    }
    if (-not $encoderAliveWhenDecoderCompleted -or $focusViolation -or $containmentViolation)
    {
        throw "Broadcast or screen-safety contract failed for $($caseDefinition.name)"
    }
    if ($resourceSamples.Count -lt 2)
    {
        throw "Insufficient resource samples for $($caseDefinition.name)"
    }

    $encoderReport = Get-Content -Raw -LiteralPath $encoderReportPath | ConvertFrom-Json
    $decoderReport = Get-Content -Raw -LiteralPath $decoderReportPath | ConvertFrom-Json
    $journalAnalysis = Get-DecoderJournalAnalysis -Path $decoderJournalPath -RunId $runId
    $expectedActualBackend = if ($caseDefinition.backend -ceq 'dxgi') { 'DXGI Desktop Duplication' } else { 'WGC' }
    $outputFiles = @(Get-ChildItem -LiteralPath $outputDirectory -File -Force)
    if ($outputFiles.Count -ne 1)
    {
        throw "Expected one published output for $($caseDefinition.name)"
    }
    $outputIdentity = Get-FileIdentity -Path $outputFiles[0].FullName -RelativeTo $resolvedOutputRoot
    if ($encoderReport.schema -cne 'PixelBridge.RunReport.2' -or $decoderReport.schema -cne 'PixelBridge.RunReport.2' -or
        $encoderReport.runId -cne $runId -or $decoderReport.runId -cne $runId -or
        $encoderReport.role -cne 'Encoder' -or $decoderReport.role -cne 'Decoder' -or
        $encoderReport.state -cne 'Stopped' -or $decoderReport.state -cne 'Completed' -or
        [string]$encoderReport.sessionId -cne [string]$decoderReport.sessionId -or
        [UInt64]$encoderReport.sessionTag -ne [UInt64]$decoderReport.sessionTag -or
        [UInt64]$encoderReport.visualProfileId -ne [UInt64]$decoderReport.visualProfileId -or
        [int]$encoderReport.visualLayoutVersion -ne [int]$decoderReport.visualLayoutVersion -or
        [string]$encoderReport.remoteMetadata.channelType -cne 'LocalDesktop' -or
        [string]$decoderReport.remoteMetadata.channelType -cne 'LocalDesktop' -or
        [string]$decoderReport.actualBackend -cne $expectedActualBackend -or
        -not [bool]$encoderReport.sourceStable -or -not [bool]$decoderReport.wholeFileDigestVerified -or
        -not [bool]$decoderReport.finalPublishSucceeded -or [Int64]$decoderReport.originalFileBytes -ne $sourceIdentity.size -or
        [Int64]$decoderReport.verifiedRawBytes -ne $sourceIdentity.size -or $outputIdentity.size -ne $sourceIdentity.size -or
        $outputIdentity.sha256 -cne $sourceIdentity.sha256 -or
        [string]$decoderReport.wholeFileDigest -notmatch '^[0-9a-f]{64}$' -or
        [string]$decoderReport.wholeFileDigest -cne [string]$encoderReport.wholeFileDigest -or
        [bool]$decoderReport.replay.enabled -or [bool]$decoderReport.replay.offlineMode -or
        [UInt64]$decoderReport.outerAdmission.conflictRejections -ne 0 -or [UInt64]$decoderReport.crcFailures -ne 0 -or
        [UInt64]$decoderReport.identityFailures -ne 0 -or -not [bool]$decoderReport.evidence.journalEnabled -or
        -not [bool]$decoderReport.evidence.valid -or [bool]$decoderReport.evidence.journalTruncated -or
        -not [bool]$decoderReport.evidence.journalFinished -or
        [UInt64]$decoderReport.evidence.journalSamples -ne [UInt64]$journalAnalysis.recordCount -or
        [UInt64]$decoderReport.outerAdmission.resourceRejections -ne [UInt64]$journalAnalysis.finalResourceRejections)
    {
        throw "Authoritative report or external publication contract failed for $($caseDefinition.name)"
    }
    $goodput = Require-Finite -Value $decoderReport.verifiedEncodedGoodputBitsPerSecond -Name "post goodput $($caseDefinition.name)" -Positive
    $fecFrameErrorRate = Require-Finite -Value $decoderReport.fecFrameErrorRate -Name "post FER $($caseDefinition.name)" -UnitInterval
    $uniqueVisualFps = Require-Finite -Value $decoderReport.uniqueVisualFps -Name "post UniqueVisualFPS $($caseDefinition.name)" -Positive
    $admittedFrameSequenceFps = Require-Finite -Value $decoderReport.admittedFrameSequenceFps -Name "post admitted FrameSequence FPS $($caseDefinition.name)" -Positive
    if ([UInt64]$decoderReport.frameLeaseHighWater -gt 6 -or [UInt64]$decoderReport.demodPendingHighWater -gt 4 -or
        [UInt64]$decoderReport.resultQueueHighWater -gt 128)
    {
        throw "A bounded queue high-water contract was exceeded for $($caseDefinition.name)"
    }

    Write-NewJsonFile -Path (Join-Path $caseRoot 'resource-samples.json') -Value @($resourceSamples)
    Write-NewJsonFile -Path (Join-Path $caseRoot 'window-samples.json') -Value @($windowSamples)
    $encoderWallSeconds = ($encoderProcess.ExitTime - $encoderProcess.StartTime).TotalSeconds
    $decoderWallSeconds = ($decoderProcess.ExitTime - $decoderProcess.StartTime).TotalSeconds
    $decoderLeadMilliseconds = ($encoderProcess.StartTime - $decoderProcess.StartTime).TotalMilliseconds
    if ($decoderLeadMilliseconds -lt $DecoderWarmupMilliseconds * 0.8)
    {
        throw "Observed Decoder lead time was shorter than the bounded warmup contract for $($caseDefinition.name)"
    }
    $caseSummary = [ordered]@{
        schema = 'PixelBridge.LocalDesktopRegressionCase.1'
        name = $caseDefinition.name
        backend = $caseDefinition.backend
        profile = $caseDefinition.profile
        runId = $runId
        source = $sourceIdentity
        output = $outputIdentity
        sourceOutputLengthEqual = $outputIdentity.size -eq $sourceIdentity.size
        sourceOutputSha256Equal = $outputIdentity.sha256 -ceq $sourceIdentity.sha256
        launch = [ordered]@{
            order = 'DecoderBeforeEncoder'
            configuredDecoderWarmupMilliseconds = $DecoderWarmupMilliseconds
            observedDecoderLeadMilliseconds = $decoderLeadMilliseconds
            decoderStartedUtc = $decoderProcess.StartTime.ToUniversalTime().ToString('O')
            encoderStartedUtc = $encoderProcess.StartTime.ToUniversalTime().ToString('O')
        }
        encoder = [ordered]@{
            processId = $encoderProcess.Id
            exitCode = $encoderProcess.ExitCode
            aliveWhenDecoderCompleted = $encoderAliveWhenDecoderCompleted
            cpuSeconds = $encoderProcess.TotalProcessorTime.TotalSeconds
            wallSeconds = $encoderWallSeconds
            cpuEquivalentCores = $encoderProcess.TotalProcessorTime.TotalSeconds / $encoderWallSeconds
            gpu = Get-ProcessGpuSummary -Samples @($resourceSamples) -ProcessId $encoderProcess.Id
            resourceHighWater = Get-ResourceHighWater -Samples @($resourceSamples) -Role encoder
            report = Get-FileIdentity -Path $encoderReportPath -RelativeTo $resolvedOutputRoot
        }
        decoder = [ordered]@{
            processId = $decoderProcess.Id
            exitCode = $decoderProcess.ExitCode
            cpuSeconds = $decoderProcess.TotalProcessorTime.TotalSeconds
            wallSeconds = $decoderWallSeconds
            cpuEquivalentCores = $decoderProcess.TotalProcessorTime.TotalSeconds / $decoderWallSeconds
            gpu = Get-ProcessGpuSummary -Samples @($resourceSamples) -ProcessId $decoderProcess.Id
            resourceHighWater = Get-ResourceHighWater -Samples @($resourceSamples) -Role decoder
            report = Get-FileIdentity -Path $decoderReportPath -RelativeTo $resolvedOutputRoot
            journal = Get-FileIdentity -Path $decoderJournalPath -RelativeTo $resolvedOutputRoot
        }
        screenSafety = [ordered]@{
            foregroundProcessIdBefore = $foregroundBefore
            foregroundProcessIdAfter = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
            focusViolation = $focusViolation
            containmentViolation = $containmentViolation
        }
        replayEnabled = [bool]$decoderReport.replay.enabled
        decoderState = [string]$decoderReport.state
        wholeFileDigestVerified = [bool]$decoderReport.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$decoderReport.finalPublishSucceeded
        verifiedEncodedGoodputBitsPerSecond = $goodput
        captureFps = [double]$decoderReport.captureFps
        uniqueVisualFps = $uniqueVisualFps
        uniqueVisualFpsBasis = [string]$decoderReport.uniqueVisualFpsBasis
        roiPixelDigestUniqueVisualFps = $decoderReport.roiPixelDigestUniqueVisualFps
        roiPixelDigestAvailability = [string]$decoderReport.roiPixelDigestAvailability
        admittedFrameSequenceFps = $admittedFrameSequenceFps
        preFecBerEstimate = $decoderReport.preFecBerEstimate
        fecFrameErrorRate = $fecFrameErrorRate
        bootstrapSuccessRate = [double]$decoderReport.bootstrapSuccessRate
        frameLeaseHighWater = [UInt64]$decoderReport.frameLeaseHighWater
        demodPendingHighWater = [UInt64]$decoderReport.demodPendingHighWater
        resultQueueHighWater = [UInt64]$decoderReport.resultQueueHighWater
        staleResultDrops = [UInt64]$decoderReport.staleResultDrops
        outerAdmission = [ordered]@{
            uniqueSymbols = [UInt64]$decoderReport.outerAdmission.uniqueSymbols
            identicalDuplicateSymbols = [UInt64]$decoderReport.outerAdmission.identicalDuplicateSymbols
            recoveryAlreadyReadySymbols = [UInt64]$decoderReport.outerAdmission.recoveryAlreadyReadySymbols
            alreadyCompletedSymbols = [UInt64]$decoderReport.outerAdmission.alreadyCompletedSymbols
            resourceRejections = [UInt64]$decoderReport.outerAdmission.resourceRejections
            conflictRejections = [UInt64]$decoderReport.outerAdmission.conflictRejections
        }
        journalAnalysis = $journalAnalysis
        encoderBroadcastRuntimeMilliseconds = [UInt64]$encoderReport.broadcastRuntimeMilliseconds
        encoderCycleCount = [UInt64]$encoderReport.cycleCount
        encoderPresentCallFps = [double]$encoderReport.presentCallFps
        encoderGeneratedVisualFramesPerSecond = [double]$encoderReport.generatedVisualFramesPerSecond
        resourceSampleCount = $resourceSamples.Count
        windowSampleCount = $windowSamples.Count
    }
    $caseSummary.artifacts = Get-CaseArtifactInventory -CaseRoot $caseRoot -EvidenceRoot $resolvedOutputRoot
    Write-NewJsonFile -Path (Join-Path $caseRoot 'case-summary.json') -Value $caseSummary
    $postCases.Add([pscustomobject]$caseSummary)
    Write-Host "PB_STEP19_CASE_PASS name=$($caseDefinition.name) goodput=$goodput uniqueVisualFps=$uniqueVisualFps"
}

$comparisonRows = [Collections.Generic.List[object]]::new()
foreach ($caseName in $expectedCases)
{
    $baselineCase = @($baselineCases | Where-Object { $_.name -ceq $caseName })[0]
    $postCase = @($postCases | Where-Object { $_.name -ceq $caseName })[0]
    $baselineGoodput = [double]$baselineCase.verifiedEncodedGoodputBitsPerSecond
    $postGoodput = [double]$postCase.verifiedEncodedGoodputBitsPerSecond
    $goodputRatio = $postGoodput / $baselineGoodput
    $baselineGpu = Get-BaselineGpuSummary -BaselineRoot (Split-Path -Parent $resolvedBaselineSummaryPath) -CaseName $caseName
    $comparisonRows.Add([pscustomobject][ordered]@{
            name = $caseName
            backend = [string]$postCase.backend
            profile = [string]$postCase.profile
            baselineGoodputBitsPerSecond = $baselineGoodput
            postGoodputBitsPerSecond = $postGoodput
            goodputRatio = $goodputRatio
            goodputChangePercent = ($goodputRatio - 1.0) * 100.0
            goodputRegressionWithinTenPercent = $goodputRatio -ge 0.9
            baselineFecFrameErrorRate = [double]$baselineCase.fecFrameErrorRate
            postFecFrameErrorRate = [double]$postCase.fecFrameErrorRate
            baselineUniqueVisualFps = $baselineCase.uniqueVisualFps
            postUniqueVisualFps = [double]$postCase.uniqueVisualFps
            baselineAdmittedFrameSequenceFps = [double]$baselineCase.admittedFrameSequenceFps
            postAdmittedFrameSequenceFps = [double]$postCase.admittedFrameSequenceFps
            baselineEncoderCpuEquivalentCores = [double]$baselineCase.encoderCpuEquivalentCores
            postEncoderCpuEquivalentCores = [double]$postCase.encoder.cpuEquivalentCores
            baselineDecoderCpuEquivalentCores = [double]$baselineCase.decoderCpuEquivalentCores
            postDecoderCpuEquivalentCores = [double]$postCase.decoder.cpuEquivalentCores
            baselineEncoderGpu = $baselineGpu.encoder
            postEncoderGpu = $postCase.encoder.gpu
            baselineDecoderGpu = $baselineGpu.decoder
            postDecoderGpu = $postCase.decoder.gpu
            baselineFrameLeaseHighWater = [UInt64]$baselineCase.frameLeaseHighWater
            postFrameLeaseHighWater = [UInt64]$postCase.frameLeaseHighWater
            baselineDemodPendingHighWater = [UInt64]$baselineCase.demodPendingHighWater
            postDemodPendingHighWater = [UInt64]$postCase.demodPendingHighWater
            baselineResultQueueHighWater = [UInt64]$baselineCase.resultQueueHighWater
            postResultQueueHighWater = [UInt64]$postCase.resultQueueHighWater
            baselineStaleResultDrops = [UInt64]$baselineCase.staleResultDrops
            postStaleResultDrops = [UInt64]$postCase.staleResultDrops
            postOuterResourceRejections = [UInt64]$postCase.outerAdmission.resourceRejections
            postDescriptorResourceRejectionIncrease = [UInt64]$postCase.journalAnalysis.postDescriptorResourceRejectionIncrease
            externalHashPass = [bool]$postCase.sourceOutputSha256Equal
            wholeFileDigestPass = [bool]$postCase.wholeFileDigestVerified
            finalPublishPass = [bool]$postCase.finalPublishSucceeded
            encoderStillBroadcastingAtCompletion = [bool]$postCase.encoder.aliveWhenDecoderCompleted
            replayOff = -not [bool]$postCase.replayEnabled
            launchAlignmentPass = [string]$postCase.launch.order -ceq 'DecoderBeforeEncoder' -and
                [double]$postCase.launch.observedDecoderLeadMilliseconds -ge
                    [double]$postCase.launch.configuredDecoderWarmupMilliseconds * 0.8 -and
                [UInt64]$postCase.journalAnalysis.postDescriptorResourceRejectionIncrease -eq 0 -and
                [UInt64]$postCase.journalAnalysis.finalConflictRejections -eq 0
            boundedHighWaterPass = [UInt64]$postCase.frameLeaseHighWater -le 6 -and
                [UInt64]$postCase.demodPendingHighWater -le 4 -and [UInt64]$postCase.resultQueueHighWater -le 128
            baselineGpuArtifacts = [ordered]@{
                resourceSamples = $baselineGpu.resourceSamples
                windowSamples = $baselineGpu.windowSamples
            }
        })
}

$allGoodputPass = @($comparisonRows | Where-Object { -not $_.goodputRegressionWithinTenPercent }).Count -eq 0
    $allCompletionPass = @($comparisonRows | Where-Object {
        -not $_.externalHashPass -or -not $_.wholeFileDigestPass -or -not $_.finalPublishPass -or
        -not $_.encoderStillBroadcastingAtCompletion -or -not $_.replayOff -or -not $_.launchAlignmentPass -or
        -not $_.boundedHighWaterPass
    }).Count -eq 0
$status = if ($allGoodputPass -and $allCompletionPass) { 'PASS' } else { 'BLOCKED' }
$postSummary = [ordered]@{
    schema = 'PixelBridge.LocalDesktopRegressionPost.1'
    status = $status
    generatedUtc = [DateTime]::UtcNow.ToString('O')
    step = 19
    preflight = Get-FileIdentity -Path (Join-Path $resolvedOutputRoot 'preflight.json') -RelativeTo $resolvedOutputRoot
    packageManifestSha256 = $packageManifestIdentity.sha256
    testedSourceFingerprintSha256 = [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256
    baselineSummarySha256 = $baselineIdentity.sha256
    source = $sourceIdentity
    protectedMonitorConcurrentWork = $ProtectedMonitorConcurrentWork
    replayOff = $true
    dataWindowAndRoi = [ordered]@{ left = [int]$roi.left; top = [int]$roi.top; right = [int]$roi.right; bottom = [int]$roi.bottom }
    launchPolicy = $preflight.launchPolicy
    matrixForegroundProcessIdBefore = $matrixForegroundBefore
    matrixForegroundProcessIdAfter = [UInt32][PbLocalDesktopRegressionWindowProbe]::ForegroundProcessId()
    cases = @($postCases)
    comparison = @($comparisonRows)
    acceptance = [ordered]@{
        fourCaseWholeFileDigestPublishExternalHash = $allCompletionPass
        encoderStillBroadcastingAllCases = @($comparisonRows | Where-Object { -not $_.encoderStillBroadcastingAtCompletion }).Count -eq 0
        replayOffAllCases = @($comparisonRows | Where-Object { -not $_.replayOff }).Count -eq 0
        decoderBeforeEncoderAndNoPostDescriptorResourceIncreaseAllCases = @($comparisonRows | Where-Object { -not $_.launchAlignmentPass }).Count -eq 0
        boundedHighWaterAllCases = @($comparisonRows | Where-Object { -not $_.boundedHighWaterPass }).Count -eq 0
        goodputRegressionWithinTenPercentAllCases = $allGoodputPass
        baselineUniqueVisualFpsAvailability = 'Unavailable in frozen RunReport.1 baseline; admitted FrameSequence FPS is compared separately and is not relabeled as pixel-digest UniqueVisualFPS'
        postRoiPixelDigestUniqueVisualFpsAvailability = 'Unavailable on the production D3D11 fast path; post uniqueVisualFps uses distinct legal visual identity'
    }
}
$postSummaryPath = Join-Path $resolvedOutputRoot 'post-summary.json'
Write-NewJsonFile -Path $postSummaryPath -Value $postSummary

$csvRows = @($comparisonRows | ForEach-Object {
        [pscustomobject][ordered]@{
            name = $_.name
            backend = $_.backend
            profile = $_.profile
            baselineGoodputBitsPerSecond = $_.baselineGoodputBitsPerSecond
            postGoodputBitsPerSecond = $_.postGoodputBitsPerSecond
            goodputChangePercent = $_.goodputChangePercent
            goodputRegressionWithinTenPercent = $_.goodputRegressionWithinTenPercent
            baselineFecFrameErrorRate = $_.baselineFecFrameErrorRate
            postFecFrameErrorRate = $_.postFecFrameErrorRate
            baselineUniqueVisualFps = $_.baselineUniqueVisualFps
            postUniqueVisualFps = $_.postUniqueVisualFps
            baselineAdmittedFrameSequenceFps = $_.baselineAdmittedFrameSequenceFps
            postAdmittedFrameSequenceFps = $_.postAdmittedFrameSequenceFps
            baselineEncoderCpuEquivalentCores = $_.baselineEncoderCpuEquivalentCores
            postEncoderCpuEquivalentCores = $_.postEncoderCpuEquivalentCores
            baselineDecoderCpuEquivalentCores = $_.baselineDecoderCpuEquivalentCores
            postDecoderCpuEquivalentCores = $_.postDecoderCpuEquivalentCores
            baselineFrameLeaseHighWater = $_.baselineFrameLeaseHighWater
            postFrameLeaseHighWater = $_.postFrameLeaseHighWater
            baselineDemodPendingHighWater = $_.baselineDemodPendingHighWater
            postDemodPendingHighWater = $_.postDemodPendingHighWater
            baselineResultQueueHighWater = $_.baselineResultQueueHighWater
            postResultQueueHighWater = $_.postResultQueueHighWater
            externalHashPass = $_.externalHashPass
            wholeFileDigestPass = $_.wholeFileDigestPass
            finalPublishPass = $_.finalPublishPass
            encoderStillBroadcastingAtCompletion = $_.encoderStillBroadcastingAtCompletion
            replayOff = $_.replayOff
            launchAlignmentPass = $_.launchAlignmentPass
            postOuterResourceRejections = $_.postOuterResourceRejections
            postDescriptorResourceRejectionIncrease = $_.postDescriptorResourceRejectionIncrease
            boundedHighWaterPass = $_.boundedHighWaterPass
        }
    })
$csvText = $csvRows | ConvertTo-Csv -NoTypeInformation
Write-NewUtf8File -Path (Join-Path $resolvedOutputRoot 'comparison.csv') -Content (($csvText -join [Environment]::NewLine) + [Environment]::NewLine)

$markdown = [Text.StringBuilder]::new()
[void]$markdown.AppendLine('# Step 19 LocalDesktop same-source regression')
[void]$markdown.AppendLine()
[void]$markdown.AppendLine("Status: **$status**")
[void]$markdown.AppendLine()
[void]$markdown.AppendLine('| Capture/Profile | Baseline Mbit/s | Post Mbit/s | Change | FER pre/post | Unique FPS pre/post | Encoder alive | Digest/publish/hash | Replay | HWM |')
[void]$markdown.AppendLine('|---|---:|---:|---:|---:|---:|---|---|---|---|')
foreach ($row in $comparisonRows)
{
    $baselineMbit = [double]$row.baselineGoodputBitsPerSecond / 1000000.0
    $postMbit = [double]$row.postGoodputBitsPerSecond / 1000000.0
    $baselineUnique = if ($null -eq $row.baselineUniqueVisualFps) { 'N/A' } else { '{0:F3}' -f [double]$row.baselineUniqueVisualFps }
    [void]$markdown.AppendLine(('| {0} | {1:F3} | {2:F3} | {3:+0.00;-0.00;0.00}% | {4:F6}/{5:F6} | {6}/{7:F3} | {8} | {9} | {10} | {11} |' -f
            $row.name, $baselineMbit, $postMbit, [double]$row.goodputChangePercent,
            [double]$row.baselineFecFrameErrorRate, [double]$row.postFecFrameErrorRate,
            $baselineUnique, [double]$row.postUniqueVisualFps,
            $(if ($row.encoderStillBroadcastingAtCompletion) { 'PASS' } else { 'FAIL' }),
            $(if ($row.externalHashPass -and $row.wholeFileDigestPass -and $row.finalPublishPass) { 'PASS' } else { 'FAIL' }),
            $(if ($row.replayOff) { 'OFF' } else { 'FAIL' }),
            $(if ($row.boundedHighWaterPass) { 'PASS' } else { 'FAIL' })))
}
[void]$markdown.AppendLine()
[void]$markdown.AppendLine('- Baseline `uniqueVisualFps` is unavailable and is not reconstructed from a different metric. The admitted FrameSequence rate remains separately present in JSON/CSV.')
[void]$markdown.AppendLine('- Post-run ROI pixel-digest UniqueVisualFPS remains unavailable on the production D3D11 fast path; the reported post metric uses distinct legal visual identity.')
[void]$markdown.AppendLine('- Launch alignment: Decoder started before Encoder, removing the prior fixed three-second mid-Carousel bias. The journal keeps cold-start acquisition inside the measurement and proves resource rejections did not increase after the first sampled descriptor-bound state.')
[void]$markdown.AppendLine(("- Protected-monitor concurrent work metadata: ``{0}``." -f $ProtectedMonitorConcurrentWork))
Write-NewUtf8File -Path (Join-Path $resolvedOutputRoot 'comparison.md') -Content $markdown.ToString()

$sealedArtifacts = @(Get-ChildItem -LiteralPath $resolvedOutputRoot -Recurse -File -Force | Sort-Object {
        [IO.Path]::GetRelativePath($resolvedOutputRoot, $_.FullName).Replace('\', '/')
    } | ForEach-Object { Get-FileIdentity -Path $_.FullName -RelativeTo $resolvedOutputRoot })
$seal = [ordered]@{
    schema = 'PixelBridge.LocalDesktopRegressionSeal.1'
    createdUtc = [DateTime]::UtcNow.ToString('O')
    status = $status
    artifactCount = $sealedArtifacts.Count
    artifacts = $sealedArtifacts
    externalBindings = [ordered]@{
        packageManifest = $packageManifestIdentity
        baselineSummary = $baselineIdentity
        source = $sourceIdentity
    }
}
$sealPath = Join-Path $resolvedOutputRoot 'evidence-seal.json'
Write-NewJsonFile -Path $sealPath -Value $seal

$result = [ordered]@{
    schema = 'PixelBridge.LocalDesktopRegressionGateResult.1'
    status = $status
    evidenceRoot = $resolvedOutputRoot
    postSummary = Get-FileIdentity -Path $postSummaryPath
    comparisonCsv = Get-FileIdentity -Path (Join-Path $resolvedOutputRoot 'comparison.csv')
    comparisonMarkdown = Get-FileIdentity -Path (Join-Path $resolvedOutputRoot 'comparison.md')
    evidenceSeal = Get-FileIdentity -Path $sealPath
}
$result | ConvertTo-Json -Depth 8
if ($status -cne 'PASS')
{
    exit 1
}
