#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PlanPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPlanSha256,

    [Parameter(Mandatory = $true)]
    [string]$EncoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$LiveDecoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OfflineDecoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [Parameter(Mandatory = $true)]
    [string]$ReplayInspectorPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet('geometry', 'signal', 'temporal', 'metric', 'scheduler')]
    [string]$FailureClassification,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)
{
    $PSNativeCommandUseErrorActionPreference = $false
}

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

function Resolve-EvidenceRoot
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $resolved = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $item = Get-Item -LiteralPath $resolved -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint))
    {
        throw "$Name must be an existing non-reparse directory"
    }
    return $resolved
}

function Resolve-EvidenceArtifact
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$AllowEmpty
    )
    if ($Identity -isnot [System.Collections.IDictionary] -or [string]::IsNullOrWhiteSpace([string]$Identity.path) -or
        [System.IO.Path]::IsPathRooted([string]$Identity.path) -or [string]$Identity.path -match '(^|/|\\)\.\.($|/|\\)' -or
        $Identity.size -is [bool] -or (-not $AllowEmpty -and [UInt64]$Identity.size -eq 0) -or
        [string]$Identity.sha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name has an invalid confined artifact identity"
    }
    $resolved = [System.IO.Path]::GetFullPath((Join-Path $Root ([string]$Identity.path).Replace('/', '\')))
    if (-not $resolved.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw "$Name escapes its endpoint evidence root"
    }
    $actual = Get-PBFileIdentity -Path $resolved
    if ([UInt64]$actual.size -ne [UInt64]$Identity.size -or [string]$actual.sha256 -cne [string]$Identity.sha256)
    {
        throw "$Name identity mismatch"
    }
    return $resolved
}

function Assert-ToolHost
{
    param(
        [Parameter(Mandatory = $true)][object]$ToolHost,
        [Parameter(Mandatory = $true)][string]$Name
    )
    Assert-PBMatrixExactKeys -Dictionary $ToolHost -ExpectedKeys @('powerShellEdition', 'powerShellVersion') -Name "$Name tool host"
    try
    {
        $version = [Version][string]$ToolHost.powerShellVersion
    }
    catch
    {
        throw "$Name has an invalid PowerShell version"
    }
    if ([string]$ToolHost.powerShellEdition -cne 'Core' -or $version.Major -lt 7)
    {
        throw "$Name was not produced by PowerShell Core 7 or newer"
    }
}

function Assert-TimestampPair
{
    param(
        [Parameter(Mandatory = $true)][object]$Utc,
        [Parameter(Mandatory = $true)][object]$UnixMilliseconds,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($UnixMilliseconds -is [bool] -or [Int64]$UnixMilliseconds -le 0)
    {
        throw "$Name has an invalid Unix timestamp"
    }
    try
    {
        $parsed = [DateTimeOffset]::ParseExact([string]$Utc, 'o', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    }
    catch
    {
        throw "$Name has an invalid UTC timestamp"
    }
    if ($parsed.Offset -ne [TimeSpan]::Zero -or $parsed.ToUnixTimeMilliseconds() -ne [Int64]$UnixMilliseconds)
    {
        throw "$Name UTC and Unix timestamps disagree"
    }
}

function Read-FailureEndpoint
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$ProcessFileName,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'DecoderLive', 'DecoderOffline')][string]$Role,
        [Parameter(Mandatory = $true)][object]$FrozenPlan
    )
    $processPath = Join-Path $Root $ProcessFileName
    $process = Read-PBBoundedJson -Path $processPath -MaximumBytes 2MB
    $plan = $FrozenPlan.value
    $expectedStatus = if ($Role -ceq 'Encoder') { 'PASS' } else { 'FAIL' }
    if ([string]$process.schema -cne 'PixelBridge.RemoteVisualPilotEndpointProcess.1' -or
        [string]$process.endpointRole -cne $Role -or [string]$process.runId -cne [string]$plan.runId -or
        [string]$process.planSha256 -cne [string]$FrozenPlan.identity.sha256 -or [string]$process.status -cne $expectedStatus)
    {
        throw "$Role endpoint process result does not represent the expected classified-failure run"
    }
    $expectedChecks = if ($Role -ceq 'Encoder')
    {
        @('exitCodeZero', 'reportExists', 'journalExists', 'manualStopConfirmed', 'reportContract', 'journalEvidenceComplete', 'monitorPreflight')
    }
    elseif ($Role -ceq 'DecoderLive')
    {
        @('exitCodeZero', 'reportExists', 'journalExists', 'reportContract', 'journalEvidenceComplete', 'wholeFileDigestAndPublish',
            'replayContract', 'publishedFileExists', 'reportArtifactPathsStable', 'replayObservationContract', 'monitorPreflight')
    }
    else
    {
        @('exitCodeZero', 'reportExists', 'journalExists', 'reportContract', 'journalEvidenceComplete', 'wholeFileDigestAndPublish',
            'replayContract', 'publishedFileExists', 'reportArtifactPathsStable', 'replayObservationContract')
    }
    $expectedArtifacts = if ($Role -ceq 'Encoder')
    {
        @('report', 'journal', 'stdout', 'stderr', 'monitorPreflight')
    }
    elseif ($Role -ceq 'DecoderLive')
    {
        @('report', 'journal', 'published', 'replay', 'stdout', 'stderr', 'monitorPreflight')
    }
    else
    {
        @('report', 'journal', 'published', 'replay', 'stdout', 'stderr')
    }
    Assert-PBMatrixExactKeys -Dictionary $process.checks -ExpectedKeys $expectedChecks -Name "$Role checks"
    Assert-PBMatrixExactKeys -Dictionary $process.artifacts -ExpectedKeys $expectedArtifacts -Name "$Role artifacts"
    Assert-ToolHost -ToolHost $process.toolHost -Name $Role
    Assert-TimestampPair -Utc $process.startedUtc -UnixMilliseconds $process.startedUnixMilliseconds -Name "$Role start"
    Assert-TimestampPair -Utc $process.endedUtc -UnixMilliseconds $process.endedUnixMilliseconds -Name "$Role end"
    Assert-TimestampPair -Utc $process.evidenceReadyUtc -UnixMilliseconds $process.evidenceReadyUnixMilliseconds -Name "$Role evidence-ready"
    if ([Int64]$process.startedUnixMilliseconds -gt [Int64]$process.endedUnixMilliseconds -or
        [Int64]$process.endedUnixMilliseconds -gt [Int64]$process.evidenceReadyUnixMilliseconds)
    {
        throw "$Role process timestamps are not monotonic"
    }
    Assert-PBMatrixIdentityEqual -Actual $process.packageManifest -Expected $plan.deployment.packageManifest -Name "$Role package manifest"
    Assert-PBMatrixIdentityEqual -Actual $process.remoteMetadata -Expected $plan.deployment.remoteMetadata -Name "$Role remote metadata"
    $application = if ($Role -ceq 'Encoder') { $plan.applications.encoder } else { $plan.applications.decoder }
    Assert-PBMatrixIdentityEqual -Actual $process.executable -Expected $application -Name "$Role executable"
    if ($Role -ceq 'Encoder')
    {
        if ([string]$process.status -cne 'PASS' -or [Int32]$process.exitCode -ne 0 -or
            -not [bool]$process.checks.manualStopConfirmed -or -not [bool]$process.checks.reportContract -or
            -not [bool]$process.checks.journalEvidenceComplete -or -not [bool]$process.checks.monitorPreflight)
        {
            throw 'Encoder must finish the failed field run with a graceful manual Stop and complete evidence'
        }
        if ([UInt64]$process.source.size -ne [UInt64]$plan.source.size -or [string]$process.source.sha256 -cne [string]$plan.source.sha256)
        {
            throw 'Encoder source identity differs from the frozen plan'
        }
    }
    else
    {
        if (-not [bool]$process.checks.exitCodeZero -or -not [bool]$process.checks.reportExists -or
            -not [bool]$process.checks.journalExists -or
            -not [bool]$process.checks.journalEvidenceComplete -or -not [bool]$process.checks.replayContract -or
            -not [bool]$process.checks.replayObservationContract -or [bool]$process.checks.wholeFileDigestAndPublish -or
            [bool]$process.checks.publishedFileExists -or [bool]$process.checks.reportContract)
        {
            throw "$Role process result does not preserve a complete, non-published failure trace"
        }
        if ($Role -ceq 'DecoderLive' -and -not [bool]$process.checks.monitorPreflight)
        {
            throw 'Live Decoder monitor preflight did not pass before the classified field run'
        }
    }
    $reportPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.report -Name "$Role report"
    $journalPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.journal -Name "$Role journal"
    $stdoutPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.stdout -Name "$Role stdout" -AllowEmpty
    $stderrPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.stderr -Name "$Role stderr" -AllowEmpty
    $replayPath = $null
    if ($null -ne $process.artifacts.replay)
    {
        $replayPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.replay -Name "$Role Replay"
    }
    $preflightPath = $null
    if ($process.artifacts.Contains('monitorPreflight'))
    {
        $preflightPath = Resolve-EvidenceArtifact -Root $Root -Identity $process.artifacts.monitorPreflight -Name "$Role monitor preflight"
        $preflight = Read-PBBoundedJson -Path $preflightPath -MaximumBytes 2MB
        if ([string]$preflight.schema -cne 'PixelBridge.RemoteVisualMonitorPreflight.1' -or
            [string]$preflight.endpointRole -cne $Role -or [string]$preflight.status -cne 'PASS')
        {
            throw "$Role monitor preflight artifact is invalid"
        }
    }
    return [ordered]@{
        root = $Root
        processPath = $processPath
        process = $process
        reportPath = $reportPath
        report = Read-PBBoundedJson -Path $reportPath -MaximumBytes 2MB
        journalPath = $journalPath
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        replayPath = $replayPath
        preflightPath = $preflightPath
    }
}

function Read-AndValidateJournal
{
    param(
        [Parameter(Mandatory = $true)][object]$Endpoint,
        [Parameter(Mandatory = $true)][string]$ExpectedRole,
        [Parameter(Mandatory = $true)][string]$ExpectedTerminalState
    )
    $item = Get-Item -LiteralPath $Endpoint.journalPath -Force -ErrorAction Stop
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or $item.Length -le 0 -or $item.Length -gt 64MB)
    {
        throw "$ExpectedRole journal is empty, oversized, or a reparse point"
    }
    $lines = @(Get-Content -LiteralPath $Endpoint.journalPath -Encoding utf8)
    if ($lines.Count -eq 0 -or $lines.Count -gt 20000)
    {
        throw "$ExpectedRole journal line count is outside the bounded contract"
    }
    $previousSequence = [Int64]-1
    $previousUnix = [Int64]-1
    $last = $null
    foreach ($line in $lines)
    {
        if ([string]::IsNullOrWhiteSpace($line))
        {
            throw "$ExpectedRole journal contains an empty line"
        }
        $entry = ConvertFrom-PBStrictJsonText -Text $line -Name "$ExpectedRole journal entry"
        if ([string]$entry.schema -cne 'PixelBridge.RunJournal.1' -or [string]$entry.role -cne $ExpectedRole -or
            [string]$entry.runId -cne [string]$Endpoint.process.runId -or [Int64]$entry.sequence -le $previousSequence -or
            [Int64]$entry.unixMs -lt $previousUnix -or [Int64]$entry.unixMs -lt [Int64]$Endpoint.process.startedUnixMilliseconds -or
            [Int64]$entry.unixMs -gt [Int64]$Endpoint.process.endedUnixMilliseconds)
        {
            throw "$ExpectedRole journal identity, order, or process-window contract failed"
        }
        $previousSequence = [Int64]$entry.sequence
        $previousUnix = [Int64]$entry.unixMs
        $last = $entry
    }
    if ($null -eq $last -or [string]$last.state -cne $ExpectedTerminalState)
    {
        throw "$ExpectedRole journal terminal state differs from its authoritative report"
    }
}

function Assert-EvidenceInventory
{
    param(
        [Parameter(Mandatory = $true)][object]$Endpoint,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'DecoderLive', 'DecoderOffline')][string]$Role
    )
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($Endpoint.processPath, $Endpoint.reportPath, $Endpoint.journalPath, $Endpoint.stdoutPath, $Endpoint.stderrPath,
        $Endpoint.replayPath, $Endpoint.preflightPath))
    {
        if ($null -ne $path)
        {
            [void]$expected.Add([System.IO.Path]::GetFullPath([string]$path))
        }
    }
    $actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $directories = [Collections.Generic.List[object]]::new()
    foreach ($item in @(Get-ChildItem -LiteralPath $Endpoint.root -Force))
    {
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
        {
            throw "$Role endpoint inventory contains a reparse point"
        }
        if ($item.PSIsContainer)
        {
            [void]$directories.Add($item)
        }
        else
        {
            [void]$actual.Add([System.IO.Path]::GetFullPath($item.FullName))
        }
    }
    if ($actual.Count -ne $expected.Count)
    {
        throw "$Role root file inventory differs from its process result"
    }
    foreach ($path in $expected)
    {
        if (-not $actual.Contains($path))
        {
            throw "$Role root inventory is missing $path"
        }
    }
    if ($Role -ceq 'Encoder')
    {
        if ($directories.Count -ne 0)
        {
            throw 'Encoder failure evidence unexpectedly contains a child directory'
        }
        return @()
    }
    if ($directories.Count -ne 1 -or [string]$directories[0].Name -cne 'published')
    {
        throw "$Role must contain exactly one bounded published directory"
    }
    $partials = [Collections.Generic.List[object]]::new()
    $publishedItems = @(Get-ChildItem -LiteralPath $directories[0].FullName -Force)
    if ($publishedItems.Count -gt 1)
    {
        throw "$Role failure retained more than one partial output"
    }
    foreach ($item in $publishedItems)
    {
        if ($item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
            -not $item.Name.EndsWith('.part', [StringComparison]::OrdinalIgnoreCase))
        {
            throw "$Role failure published a final file or retained an unsafe output artifact"
        }
        [void]$partials.Add((Get-PBFileIdentity -Path $item.FullName))
    }
    return @($partials)
}

function Assert-EncoderFailureRunReport
{
    param(
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][string]$Source
    )
    $expectedMonitorStatus = if ([string]$Plan.profileToken -ceq 'remote-lf4') { 'PASS' } else { 'NotRequired' }
    if ([string]$Report.schema -cne 'PixelBridge.RunReport.2' -or [string]$Report.role -cne 'Encoder' -or
        [string]$Report.runId -cne [string]$Plan.runId -or [string]$Report.state -cne 'Stopped' -or
        [string]$Report.profile -cne [string]$Plan.profileName -or [UInt64]$Report.visualProfileId -ne [UInt64]$Plan.visualProfileId -or
        [UInt32]$Report.visualLayoutVersion -ne [UInt32]$Plan.visualLayoutVersion -or [UInt64]$Report.fileBytes -ne [UInt64]$Plan.source.size -or
        [UInt32]$Report.configuredLogicalVisualFps -ne [UInt32]$Plan.logicalFps -or
        [UInt32]$Report.configuredControlRepetitions -ne [UInt32]$Plan.policy.controlRepetitions -or
        [System.IO.Path]::GetFullPath([string]$Report.sourcePath) -ine [System.IO.Path]::GetFullPath($Source) -or
        $Report.sourceStable -isnot [bool] -or -not [bool]$Report.sourceStable -or [string]$Report.errorDetail -cne '' -or
        [string]$Report.monitorSafety.status -cne $expectedMonitorStatus -or [UInt64]$Report.submittedFrames -eq 0)
    {
        throw 'Encoder report does not prove a stable, manually stopped broadcast for the failed field run'
    }
    if ($Report.evidence.journalEnabled -isnot [bool] -or -not [bool]$Report.evidence.journalEnabled -or
        $Report.evidence.valid -isnot [bool] -or -not [bool]$Report.evidence.valid -or
        [bool]$Report.evidence.journalTruncated -or -not [bool]$Report.evidence.journalFinished -or
        [UInt64]$Report.evidence.journalSamples -eq 0)
    {
        throw 'Encoder report journal evidence is incomplete'
    }
}

function Assert-DecoderFailureRunReport
{
    param(
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][bool]$Offline,
        [Parameter(Mandatory = $true)][string]$ExpectedReplayPath
    )
    if ([string]$Report.schema -cne 'PixelBridge.RunReport.2' -or [string]$Report.role -cne 'Decoder' -or
        [string]$Report.runId -cne [string]$Plan.runId -or [string]$Report.state -notin @('Failed', 'Stopped') -or
        [string]$Report.profile -cne [string]$Plan.profileName -or [UInt64]$Report.visualProfileId -ne [UInt64]$Plan.visualProfileId -or
        [UInt32]$Report.visualLayoutVersion -ne [UInt32]$Plan.visualLayoutVersion -or
        $Report.wholeFileDigestVerified -isnot [bool] -or [bool]$Report.wholeFileDigestVerified -or
        $Report.finalPublishSucceeded -isnot [bool] -or [bool]$Report.finalPublishSucceeded -or
        -not [string]::IsNullOrEmpty([string]$Report.outputPath) -or [UInt64]$Report.outerAdmission.conflictRejections -ne 0)
    {
        throw "Decoder report violates the non-published failure contract (offline=$Offline)"
    }
    if ($null -ne $Report.falseAcceptedCodewords -and
        ($Report.falseAcceptedCodewords -is [bool] -or [Int64]$Report.falseAcceptedCodewords -ne 0))
    {
        throw "Decoder failure report contains false accepted codewords (offline=$Offline)"
    }
    if ($Report.evidence.journalEnabled -isnot [bool] -or -not [bool]$Report.evidence.journalEnabled -or
        $Report.evidence.valid -isnot [bool] -or -not [bool]$Report.evidence.valid -or
        [bool]$Report.evidence.journalTruncated -or -not [bool]$Report.evidence.journalFinished -or
        [UInt64]$Report.evidence.journalSamples -eq 0 -or $Report.replay.enabled -isnot [bool] -or
        -not [bool]$Report.replay.enabled -or -not [bool]$Report.replay.evidenceValid -or -not [bool]$Report.replay.finalized -or
        [bool]$Report.replay.offlineMode -ne $Offline -or
        [System.IO.Path]::GetFullPath([string]$Report.replay.path) -ine [System.IO.Path]::GetFullPath($ExpectedReplayPath) -or
        [UInt32]$Report.frameLeaseHighWater -gt 4 -or [UInt32]$Report.demodPendingHighWater -gt 8 -or
        [UInt32]$Report.resultQueueHighWater -gt 8)
    {
        throw "Decoder failure report evidence/Replay/queue contract failed (offline=$Offline)"
    }
    if (-not $Offline)
    {
        $expectedBackend = if ([string]$Plan.policy.captureBackend -ceq 'wgc') { 'WGC' } else { 'DXGI' }
        if ([string]$Report.actualBackend -cne $expectedBackend -or -not [bool]$Report.monitorSafety.preflightPassed -or
            [string]$Report.monitorSafety.status -cne 'PASS' -or [UInt64]$Report.replay.writtenFrames -eq 0 -or
            [UInt64]$Report.replay.droppedFrames -ne 0 -or [UInt64]$Report.replay.writtenDemodObservations -ne 0 -or
            [UInt64]$Report.replay.droppedDemodObservations -ne 0 -or [UInt32]$Report.replay.queueHighWater -gt 2 -or
            [UInt32]$Report.replay.maximumCaptureFramesPerSecond -ne [UInt32]$Plan.policy.replay.maximumCaptureFramesPerSecond)
        {
            throw 'Live Decoder failure report did not preserve the bounded capture/Replay contract'
        }
    }
    elseif ([string]$Report.monitorSafety.status -cne 'NotApplicableOfflineReplay' -or
        [UInt64]$Report.replay.offlineCaptureFrames -eq 0 -or
        [UInt64]$Report.replay.offlineDemodResults -ne [UInt64]$Report.replay.offlineCaptureFrames -or
        [UInt64]$Report.replay.offlineObservationComparisons -ne 0 -or
        [UInt64]$Report.replay.offlineObservationMismatches -ne 0)
    {
        throw 'Offline Decoder failure report did not reproduce the exact live Replay through production demodulation'
    }
}

$frozenPlan = Import-PBRemoteVisualPilotPlan -Path $PlanPath -ExpectedSha256 $ExpectedPlanSha256
$plan = $frozenPlan.value
if ([string]$plan.schema -cne 'PixelBridge.RemoteVisualPilotPlan.2')
{
    throw 'Classified field failures are a Step 21 PilotPlan.2 contract'
}
$encoderRoot = Resolve-EvidenceRoot -Path $EncoderEvidenceDirectory -Name 'Encoder evidence'
$liveRoot = Resolve-EvidenceRoot -Path $LiveDecoderEvidenceDirectory -Name 'live Decoder evidence'
$offlineRoot = Resolve-EvidenceRoot -Path $OfflineDecoderEvidenceDirectory -Name 'offline Decoder evidence'
if (([Collections.Generic.HashSet[string]]::new([string[]]@($encoderRoot, $liveRoot, $offlineRoot), [StringComparer]::OrdinalIgnoreCase)).Count -ne 3)
{
    throw 'Encoder/live/offline endpoint evidence directories must be independent'
}
$resolvedSource = [System.IO.Path]::GetFullPath($SourcePath)
$sourceIdentity = Get-PBFileIdentity -Path $resolvedSource
if ([UInt64]$sourceIdentity.size -ne [UInt64]$plan.source.size -or [string]$sourceIdentity.sha256 -cne [string]$plan.source.sha256)
{
    throw 'Failure evidence source differs from the frozen plan'
}
$resolvedPython = [System.IO.Path]::GetFullPath($PythonPath)
$resolvedInspector = [System.IO.Path]::GetFullPath($ReplayInspectorPath)
if (-not (Test-Path -LiteralPath $resolvedPython -PathType Leaf) -or -not (Test-Path -LiteralPath $resolvedInspector -PathType Leaf))
{
    throw 'Python or PBRemoteVisualReplayInspector executable is missing'
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrEmpty($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container) -or
    (Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath "$resolvedOutput.partial"))
{
    throw 'Failure verification output must be a new path below an existing parent'
}
if ($resolvedOutput.StartsWith($encoderRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedOutput.StartsWith($liveRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedOutput.StartsWith($offlineRoot + '\', [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Independent failure verification output must remain outside endpoint evidence roots'
}

$encoder = Read-FailureEndpoint -Root $encoderRoot -ProcessFileName 'encoder-process-result.json' -Role Encoder -FrozenPlan $frozenPlan
$live = Read-FailureEndpoint -Root $liveRoot -ProcessFileName 'live-decoder-process-result.json' -Role DecoderLive -FrozenPlan $frozenPlan
$offline = Read-FailureEndpoint -Root $offlineRoot -ProcessFileName 'offline-decoder-process-result.json' -Role DecoderOffline -FrozenPlan $frozenPlan
if ($null -eq $live.replayPath -or $null -ne $offline.replayPath -or $null -eq $offline.process.replayInput)
{
    throw 'Failure evidence requires one live Replay and an offline exact-input identity'
}
$liveReplayIdentity = Get-PBFileIdentity -Path $live.replayPath
Assert-PBMatrixIdentityEqual -Actual $offline.process.replayInput -Expected $liveReplayIdentity -Name 'offline exact Replay input'
Assert-EncoderFailureRunReport -Report $encoder.report -Plan $plan -Source $resolvedSource
Assert-DecoderFailureRunReport -Report $live.report -Plan $plan -Offline $false -ExpectedReplayPath $live.replayPath
Assert-DecoderFailureRunReport -Report $offline.report -Plan $plan -Offline $true -ExpectedReplayPath $live.replayPath
Read-AndValidateJournal -Endpoint $encoder -ExpectedRole Encoder -ExpectedTerminalState ([string]$encoder.report.state)
Read-AndValidateJournal -Endpoint $live -ExpectedRole Decoder -ExpectedTerminalState ([string]$live.report.state)
Read-AndValidateJournal -Endpoint $offline -ExpectedRole Decoder -ExpectedTerminalState ([string]$offline.report.state)
$encoderPartials = @(Assert-EvidenceInventory -Endpoint $encoder -Role Encoder)
$livePartials = @(Assert-EvidenceInventory -Endpoint $live -Role DecoderLive)
$offlinePartials = @(Assert-EvidenceInventory -Endpoint $offline -Role DecoderOffline)
if ($encoderPartials.Count -ne 0)
{
    throw 'Encoder retained an unexpected partial output'
}

New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
$inspectionPath = Join-Path $resolvedOutput 'replay-inspection.json'
$inspectionStdout = Join-Path $resolvedOutput 'replay-inspector.stdout.log'
$inspectionStderr = Join-Path $resolvedOutput 'replay-inspector.stderr.log'
& $resolvedInspector --input $live.replayPath --output $inspectionPath `
    --max-frames ([string]$plan.policy.replay.maximumFrames) --max-mib ([string]$plan.policy.replay.maximumMiB) `
    1> $inspectionStdout 2> $inspectionStderr
if ($LASTEXITCODE -ne 0)
{
    throw "PBRemoteVisualReplayInspector failed with exit code $LASTEXITCODE"
}
$inspection = Read-PBBoundedJson -Path $inspectionPath -MaximumBytes 32MB
$expectedProfileIdHex = ([UInt64]$plan.visualProfileId).ToString('x16', [Globalization.CultureInfo]::InvariantCulture)
if ([string]$inspection.schema -cne 'PixelBridge.RemoteVisualReplayInspection.1' -or
    [string]$inspection.descriptor.runId -cne [string]$plan.runId -or
    [string]$inspection.descriptor.visualProfileId -cne $expectedProfileIdHex -or
    [UInt32]$inspection.descriptor.visualLayoutVersion -ne [UInt32]$plan.visualLayoutVersion -or
    $inspection.reader.complete -isnot [bool] -or -not [bool]$inspection.reader.complete -or
    [UInt64]$inspection.reader.captureFrames -eq 0 -or [UInt64]$inspection.reader.captureFrames -ne [UInt64]$live.report.replay.writtenFrames)
{
    throw 'Replay inspection does not match the frozen plan or finalized live Replay'
}
$support = Get-PBRemoteVisualFailureSupport -Inspection $inspection -EncoderReport $encoder.report `
    -LiveDecoderReport $live.report -OfflineDecoderReport $offline.report
if ($support[$FailureClassification].supported -isnot [bool] -or -not [bool]$support[$FailureClassification].supported)
{
    throw "Selected failure classification '$FailureClassification' is not supported by Replay/report evidence"
}

$combinedDirectory = Join-Path $resolvedOutput 'combined'
$reportTool = Join-Path (Split-Path -Parent $PSScriptRoot) 'PBRemoteVisualReport\pb_remote_visual_report.py'
if (-not (Test-Path -LiteralPath $reportTool -PathType Leaf))
{
    throw 'Strict RemoteVisual report merger is missing'
}
$artifactPaths = @(
    $frozenPlan.path, [string]$plan.deployment.manifest.path, [string]$plan.remoteUi.evidence.path,
    [string]$plan.remoteUi.screenshot.path, [string]$plan.remoteUi.captureRecord.path,
    [string]$plan.deployment.remoteMetadata.path, [string]$plan.deployment.encoderEnvironment.path,
    [string]$plan.deployment.decoderEnvironment.path, [string]$plan.deployment.packageManifest.path,
    [string]$plan.deployment.sourceManifest.path, $encoder.processPath, $live.processPath, $offline.processPath,
    $offline.reportPath, $encoder.preflightPath, $live.preflightPath, $inspectionPath)
$arguments = @('-B', $reportTool, '--encoder', $encoder.reportPath, '--decoder', $live.reportPath,
    '--output-dir', $combinedDirectory, '--source-file', $resolvedSource, '--replay', $live.replayPath,
    '--decoder-clock-offset-ms', [string]$plan.policy.clock.decoderOffsetMilliseconds,
    '--clock-uncertainty-ms', [string]$plan.policy.clock.uncertaintyMilliseconds)
foreach ($path in $artifactPaths)
{
    $arguments += @('--artifact', [System.IO.Path]::GetFullPath([string]$path))
}
$reportStdout = Join-Path $resolvedOutput 'report-merger.stdout.log'
$reportStderr = Join-Path $resolvedOutput 'report-merger.stderr.log'
& $resolvedPython @arguments 1> $reportStdout 2> $reportStderr
if ($LASTEXITCODE -ne 0)
{
    throw "Strict RemoteVisual failure report merge failed with exit code $LASTEXITCODE"
}
$combinedPath = Join-Path $combinedDirectory "remote-run-$($plan.runId)-combined.json"
$combined = Read-PBBoundedJson -Path $combinedPath -MaximumBytes 8MB
if ([string]$combined.runId -cne [string]$plan.runId -or $combined.successfulRun -isnot [bool] -or
    [bool]$combined.successfulRun -or $combined.evidenceValid -isnot [bool] -or -not [bool]$combined.evidenceValid -or
    $combined.externalVerification.match -ne $null)
{
    throw 'Strict combined report did not preserve the authoritative non-published failure outcome'
}

$verification = [ordered]@{
    schema = 'PixelBridge.RemoteVisualFieldFailureVerification.1'
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PASS'
    runId = [string]$plan.runId
    profileToken = [string]$plan.profileToken
    captureBackend = [string]$plan.policy.captureBackend
    matrix = $plan.matrix
    logicalFps = [UInt32]$plan.logicalFps
    geometryMode = [string]$plan.geometryMode
    failureClassification = $FailureClassification
    classificationSupport = $support
    noFalseOutput = [ordered]@{
        wholeFileDigestVerified = $false
        finalPublishSucceeded = $false
        livePublishedFinalFiles = 0
        offlinePublishedFinalFiles = 0
        liveRetainedPartials = $livePartials.Count
        offlineRetainedPartials = $offlinePartials.Count
        falseAcceptedCodewords = $live.report.falseAcceptedCodewords
        outerConflictRejections = [UInt64]$live.report.outerAdmission.conflictRejections
    }
    plan = $frozenPlan.identity
    replay = $liveReplayIdentity
    replayInspection = Get-PBFileIdentity -Path $inspectionPath
    endpointProcessResults = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoder.processPath
        liveDecoder = Get-PBFileIdentity -Path $live.processPath
        offlineDecoder = Get-PBFileIdentity -Path $offline.processPath
    }
    endpointReports = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoder.reportPath
        liveDecoder = Get-PBFileIdentity -Path $live.reportPath
        offlineDecoder = Get-PBFileIdentity -Path $offline.reportPath
    }
    combinedReport = Get-PBFileIdentity -Path $combinedPath
    retainedPartialOutputs = [ordered]@{ live = @($livePartials); offline = @($offlinePartials) }
    replayInspector = Get-PBFileIdentity -Path $resolvedInspector
    reportMerger = Get-PBFileIdentity -Path $reportTool
}
$verificationPath = Join-Path $resolvedOutput 'failure-evidence-verification.json'
[void](Write-PBCreateOnlyJson -Path $verificationPath -Value $verification -Depth 40)
$record = New-PBRemoteVisualMatrixRunRecordValue -FrozenPlan $frozenPlan -Outcome Failure `
    -FailureClassification $FailureClassification -SourcePath $resolvedSource -ReplayPath $live.replayPath `
    -CombinedReportPath $combinedPath -EncoderReportPath $encoder.reportPath -LiveDecoderReportPath $live.reportPath `
    -OfflineDecoderReportPath $offline.reportPath -OutcomeVerificationPath $verificationPath -InspectionPath $inspectionPath
$recordPath = Join-Path $resolvedOutput 'matrix-run-record.json'
[void](Write-PBCreateOnlyJson -Path $recordPath -Value $record -Depth 40)
[void](Import-PBRemoteVisualMatrixRunRecord -Path $recordPath)

$sealInputs = [Collections.Generic.List[object]]::new()
$sealPaths = @($frozenPlan.path, [string]$plan.deployment.manifest.path, [string]$plan.remoteUi.evidence.path,
    [string]$plan.remoteUi.screenshot.path, [string]$plan.remoteUi.captureRecord.path,
    [string]$plan.deployment.remoteMetadata.path, [string]$plan.deployment.encoderEnvironment.path,
    [string]$plan.deployment.decoderEnvironment.path, [string]$plan.deployment.packageManifest.path,
    [string]$plan.deployment.sourceManifest.path, $resolvedSource, $encoder.processPath, $encoder.reportPath,
    $encoder.journalPath, $encoder.stdoutPath, $encoder.stderrPath, $encoder.preflightPath, $live.processPath,
    $live.reportPath, $live.journalPath, $live.stdoutPath, $live.stderrPath, $live.preflightPath, $live.replayPath,
    $offline.processPath, $offline.reportPath, $offline.journalPath, $offline.stdoutPath, $offline.stderrPath,
    $inspectionPath, $inspectionStdout, $inspectionStderr, $combinedPath,
    (Join-Path $combinedDirectory "remote-run-$($plan.runId).md"),
    (Join-Path $combinedDirectory "remote-run-$($plan.runId).csv"),
    $reportStdout, $reportStderr, $verificationPath, $recordPath, $resolvedInspector, $reportTool)
foreach ($partial in @($livePartials) + @($offlinePartials))
{
    $sealPaths += [string]$partial.path
}
foreach ($path in $sealPaths)
{
    [void]$sealInputs.Add((Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath([string]$path))))
}
$seal = [ordered]@{
    schema = 'PixelBridge.RemoteVisualFieldFailureSeal.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    runId = [string]$plan.runId
    status = 'PASS'
    failureClassification = $FailureClassification
    profileToken = [string]$plan.profileToken
    captureBackend = [string]$plan.policy.captureBackend
    modeClass = [string]$plan.matrix.modeClass
    geometryMode = [string]$plan.geometryMode
    logicalFps = [UInt32]$plan.logicalFps
    artifactCount = $sealInputs.Count
    artifacts = @($sealInputs)
}
$sealPath = Join-Path $resolvedOutput 'failure-evidence-seal.json'
[void](Write-PBCreateOnlyJson -Path $sealPath -Value $seal -Depth 40)
[ordered]@{
    path = $resolvedOutput
    runId = [string]$plan.runId
    status = 'PASS'
    outcome = 'Failure'
    failureClassification = $FailureClassification
    replaySha256 = [string]$liveReplayIdentity.sha256
    inspectionSha256 = [string](Get-PBFileIdentity -Path $inspectionPath).sha256
    recordSha256 = [string](Get-PBFileIdentity -Path $recordPath).sha256
    sealSha256 = [string](Get-PBFileIdentity -Path $sealPath).sha256
} | ConvertTo-Json -Depth 10
