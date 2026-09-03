[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,

    [Parameter(Mandatory = $true)]
    [string]$KitManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedGitCommit,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedSourceSha256,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{32}$')]
    [string]$ComputerARunId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{32}$')]
    [string]$ComputerBRunId,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1') -Force

function Require-Condition
{
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition)
    {
        throw $Message
    }
}

function Get-FullPath
{
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-IsWithin
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $fullPath = (Get-FullPath -Path $Path).TrimEnd('\')
    $fullRoot = (Get-FullPath -Path $Root).TrimEnd('\')
    return $fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Require-ExactExitCodeZero
{
    param([Parameter(Mandatory = $true)][string]$Path)
    $identity = Get-PBFileIdentity -Path $Path
    Require-Condition -Condition ([UInt64]$identity.size -le 16) -Message "Exit-code file is oversized: $Path"
    $text = [System.IO.File]::ReadAllText($identity.path).Trim()
    Require-Condition -Condition ($text -ceq '0') -Message "Process exit code is not zero: $Path"
    return $identity
}

function Read-RunJournal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$TerminalState,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedSamples,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedBytes
    )
    $identity = Get-PBFileIdentity -Path $Path
    Require-Condition -Condition ([UInt64]$identity.size -gt 0 -and [UInt64]$identity.size -le 64MB) `
        -Message "Run journal is empty or oversized: $Path"
    Require-Condition -Condition ([UInt64]$identity.size -eq $ExpectedBytes) `
        -Message "Run journal byte count disagrees with report: $Path"
    $lines = [System.IO.File]::ReadAllLines($identity.path)
    Require-Condition -Condition ($lines.Count -gt 0 -and $lines.Count -le 100000 -and [UInt64]$lines.Count -eq $ExpectedSamples) `
        -Message "Run journal sample count disagrees with report: $Path"
    $previousUnixMilliseconds = [Int64]::MinValue
    $first = $null
    $last = $null
    for ($index = 0; $index -lt $lines.Count; $index++)
    {
        Require-Condition -Condition (-not [string]::IsNullOrWhiteSpace($lines[$index])) `
            -Message "Run journal contains an empty record: $Path"
        $record = ConvertFrom-PBStrictJsonText -Text $lines[$index] -Name "$Role journal record $index"
        Require-Condition -Condition ([string]$record.schema -ceq 'PixelBridge.RunJournal.1' -and
            [string]$record.role -ceq $Role -and [string]$record.runId -ceq $RunId) `
            -Message "Run journal record identity mismatch: $Path"
        $unixMilliseconds = [Int64]$record.unixMs
        Require-Condition -Condition ($unixMilliseconds -ge $previousUnixMilliseconds) `
            -Message "Run journal timestamps are not monotonic: $Path"
        $previousUnixMilliseconds = $unixMilliseconds
        if ($index -eq 0)
        {
            $first = $record
        }
        $last = $record
    }
    Require-Condition -Condition ([string]$last.state -ceq $TerminalState) `
        -Message "Run journal does not end in ${TerminalState}: $Path"
    return [ordered]@{ identity = $identity; first = $first; last = $last; samples = [UInt64]$lines.Count }
}

function Require-ProfileIdentity
{
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Name
    )
    Require-Condition -Condition ([string]$Report.profile -ceq 'PB-RemoteVisual-LF4-X1 (Experimental)' -and
        [string]$Report.visualProfileId -ceq '5783275402097472561' -and
        [UInt32]$Report.visualLayoutVersion -eq 7 -and [UInt32]$Report.codedDataBytesPerFrame -eq 8100 -and
        [UInt32]$Report.codewordsPerFrame -eq 4 -and [string]$Report.compressionCodec -ceq 'RAW' -and
        [string]$Report.outerFec -ceq 'Wirehair V2' -and
        [string]$Report.innerFec -ceq 'Robust DVB-S2 Short QC-LDPC') `
        -Message "$Name profile/FEC identity mismatch"
}

function Require-ReceiverSuccess
{
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][UInt64]$SourceBytes
    )
    Require-Condition -Condition ([string]$Report.state -ceq 'Completed' -and [bool]$Report.descriptorKnown -and
        [UInt64]$Report.originalFileBytes -eq $SourceBytes -and [UInt64]$Report.verifiedRawBytes -eq $SourceBytes -and
        [UInt64]$Report.remainingRawBytes -eq 0 -and [double]$Report.recoveryProgress -eq 1.0 -and
        [bool]$Report.wholeFileDigestVerified -and [bool]$Report.finalPublishSucceeded) `
        -Message "$Name did not complete Receiver, WholeFileDigest, and safe publish"
    Require-Condition -Condition ([UInt64]$Report.acceptedTransportBlocks -ge [UInt64]$Report.outerAdmission.uniqueSymbols -and
        [UInt64]$Report.outerAdmission.uniqueSymbols -gt 0 -and [UInt64]$Report.outerAdmission.recoveryReadyEvents -eq 1 -and
        [UInt64]$Report.fecFailures -eq 0 -and [UInt64]$Report.crcFailures -eq 0 -and
        [UInt64]$Report.identityFailures -eq 0 -and
        [UInt64]$Report.outerAdmission.resourceRejections -eq 0 -and
        [UInt64]$Report.outerAdmission.conflictRejections -eq 0) `
        -Message "$Name contains invalid Receiver/Outer acceptance or a closed failure"
    Require-Condition -Condition ($Report.falseAcceptedCodewords -eq $null -and
        -not [string]::IsNullOrWhiteSpace([string]$Report.falseAcceptedCodewordsUnavailableReason)) `
        -Message "$Name must preserve the unavailable production false-acceptance truth boundary"
    Require-Condition -Condition ([bool]$Report.evidence.valid -and -not [bool]$Report.evidence.journalTruncated -and
        [bool]$Report.evidence.journalFinished -and [string]::IsNullOrEmpty([string]$Report.evidence.invalidReason)) `
        -Message "$Name run evidence is incomplete or invalid"
}

$resolvedRoot = (Get-FullPath -Path $EvidenceRoot).TrimEnd('\')
Require-Condition -Condition (Test-Path -LiteralPath $resolvedRoot -PathType Container) `
    -Message 'Step 21 single-run evidence root is missing'
$resolvedOutput = Get-FullPath -Path $OutputPath
Require-Condition -Condition (Test-IsWithin -Path $resolvedOutput -Root $resolvedRoot) `
    -Message 'Step 21 acceptance output must remain inside the evidence root'
Require-Condition -Condition (-not (Test-Path -LiteralPath $resolvedOutput)) `
    -Message 'Step 21 acceptance output is create-only and already exists'

$kitManifest = Read-PBBoundedJson -Path $KitManifestPath -MaximumBytes 8MB
$sourceIdentity = Get-PBFileIdentity -Path $SourcePath
Require-Condition -Condition ([string]$kitManifest.schema -ceq 'PixelBridge.RemoteVisualStep21ComputerBKit.1' -and
    [string]$kitManifest.headCommit -ceq $ExpectedGitCommit -and
    [string]$kitManifest.launchers.demoMonitorSelector -ceq 'primary' -and
    -not [bool]$kitManifest.truthBoundary.formalStep21Accepted -and
    -not [bool]$kitManifest.truthBoundary.certifiedRemoteVisualProfile) `
    -Message 'Computer B kit identity or predeployment truth boundary mismatch'
Require-Condition -Condition ([UInt64]$kitManifest.sourceSet.demoSource.size -eq [UInt64]$sourceIdentity.size -and
    [string]$kitManifest.sourceSet.demoSource.sha256 -ceq $ExpectedSourceSha256 -and
    [string]$sourceIdentity.sha256 -ceq $ExpectedSourceSha256) `
    -Message 'Sealed Computer B source identity mismatch'

$routeDecisionPath = Join-Path $resolvedRoot 'step21-route-decision.json'
$routeDecision = Read-PBBoundedJson -Path $routeDecisionPath -MaximumBytes 1MB
Require-Condition -Condition ([string]$routeDecision.schema -ceq 'PixelBridge.RemoteVisualStep21RouteDecision.1' -and
    [string]$routeDecision.decisionAuthority -ceq 'ExplicitUserSelectionInActiveGoal' -and
    [string]$routeDecision.selectedOption -ceq 'CloseWithOneSuccessfulCompleteFileRealMachineRun' -and
    [string]$routeDecision.computerARunId -ceq $ComputerARunId -and
    [string]$routeDecision.computerBRunId -ceq $ComputerBRunId -and
    [bool]$routeDecision.crossBrandMatrixWaived -and [bool]$routeDecision.crossModeMatrixWaived -and
    -not [bool]$routeDecision.crossBrandCoverageClaimed -and -not [bool]$routeDecision.crossModeCoverageClaimed -and
    -not [bool]$routeDecision.certifiedRemoteVisualProfile) `
    -Message 'Step 21 user-authorized route decision mismatch'

$encoderReportPath = Join-Path $resolvedRoot 'computer-b-evidence\encoder-report.json'
$encoderJournalPath = Join-Path $resolvedRoot 'computer-b-evidence\encoder-journal.ndjson'
$liveReportPath = Join-Path $resolvedRoot 'decoder-report.json'
$liveJournalPath = Join-Path $resolvedRoot 'decoder-journal.ndjson'
$liveExitCodePath = Join-Path $resolvedRoot 'decoder-exit-code.txt'
$replayPath = Join-Path $resolvedRoot 'live-replay.pbrr'
$offlineRoot = Join-Path $resolvedRoot 'offline-replay'
$offlineReportPath = Join-Path $offlineRoot 'decoder-replay-report.json'
$offlineJournalPath = Join-Path $offlineRoot 'decoder-replay-journal.ndjson'
$offlineExitCodePath = Join-Path $offlineRoot 'decoder-replay-exit-code.txt'

$encoderReport = Read-PBBoundedJson -Path $encoderReportPath -MaximumBytes 2MB
$liveReport = Read-PBBoundedJson -Path $liveReportPath -MaximumBytes 4MB
$offlineReport = Read-PBBoundedJson -Path $offlineReportPath -MaximumBytes 4MB
foreach ($binding in @(
    [ordered]@{ report = $encoderReport; role = 'Encoder'; runId = $ComputerBRunId },
    [ordered]@{ report = $liveReport; role = 'Decoder'; runId = $ComputerARunId },
    [ordered]@{ report = $offlineReport; role = 'Decoder'; runId = $ComputerARunId }))
{
    Require-Condition -Condition ([string]$binding.report.schema -ceq 'PixelBridge.RunReport.2' -and
        [string]$binding.report.applicationName -ceq "PixelBridge$($binding.role)" -and
        [string]$binding.report.role -ceq [string]$binding.role -and
        [string]$binding.report.runId -ceq [string]$binding.runId -and
        [string]$binding.report.gitCommit -ceq $ExpectedGitCommit) `
        -Message "$($binding.role) report identity mismatch"
    Require-ProfileIdentity -Report $binding.report -Name "$($binding.role) report"
}

Require-Condition -Condition ([string]$encoderReport.state -ceq 'Stopped' -and [bool]$encoderReport.sourceStable -and
    [UInt64]$encoderReport.fileBytes -eq [UInt64]$sourceIdentity.size -and
    [bool]$encoderReport.dataWindow.singleMonitorFullscreen -and [UInt32]$encoderReport.configuredLogicalVisualFps -eq 2 -and
    [UInt64]$encoderReport.logicalDwellViolationCount -eq 0 -and
    [double]$encoderReport.generatedVisualFramesPerSecond -gt 0 -and
    [double]$encoderReport.generatedVisualFramesPerSecond -le 2.1 -and
    [bool]$encoderReport.evidence.valid -and -not [bool]$encoderReport.evidence.journalTruncated -and
    [bool]$encoderReport.evidence.journalFinished -and
    [string]$encoderReport.monitorSafety.status -ceq 'NotApplicableSingleMonitorFullscreen' -and
    [string]$encoderReport.statusMessage -ceq 'Broadcast stopped by user; no sender-side receiver completion was inferred') `
    -Message 'Computer B Encoder did not satisfy the authorized single-monitor sender contract'

Require-ReceiverSuccess -Report $liveReport -Name 'Live Decoder' -SourceBytes ([UInt64]$sourceIdentity.size)
Require-ReceiverSuccess -Report $offlineReport -Name 'Offline Decoder' -SourceBytes ([UInt64]$sourceIdentity.size)
Require-Condition -Condition ([string]$liveReport.actualBackend -ceq 'WGC' -and
    [bool]$liveReport.monitorSafety.preflightPassed -and [string]$liveReport.monitorSafety.status -ceq 'PASS' -and
    [Int32]$liveReport.roi.left -eq 2560 -and [Int32]$liveReport.roi.top -eq 0 -and
    [Int32]$liveReport.roi.width -eq 2560 -and [Int32]$liveReport.roi.height -eq 1440 -and
    [UInt64]$liveReport.captureArrivedFrames -gt 0 -and [double]$liveReport.uniqueVisualFps -gt 0 -and
    [double]$liveReport.uniqueVisualFps -le 5.1 -and
    [string]$liveReport.observedLocatorGeometry.authority -ceq 'AcceptedBootstrapLocatorPixels' -and
    [UInt64]$liveReport.observedLocatorGeometry.samples -gt 0 -and
    [double]$liveReport.observedLocatorGeometry.minimumScaleX -ge 0.5 -and
    [double]$liveReport.observedLocatorGeometry.maximumScaleX -le 2.0 -and
    [double]$liveReport.observedLocatorGeometry.minimumScaleY -ge 0.5 -and
    [double]$liveReport.observedLocatorGeometry.maximumScaleY -le 2.0 -and
    [string]$liveReport.remoteMetadata.remoteProvider -ceq 'UserProvidedVisualLink') `
    -Message 'Live Decoder did not use the expected right-screen WGC LF4 pixel path'

$replayIdentity = Get-PBFileIdentity -Path $replayPath
Require-Condition -Condition ([bool]$liveReport.replay.enabled -and -not [bool]$liveReport.replay.offlineMode -and
    [bool]$liveReport.replay.evidenceValid -and [bool]$liveReport.replay.finalized -and
    [UInt64]$liveReport.replay.writtenFrames -gt 0 -and [UInt64]$liveReport.replay.droppedFrames -eq 0 -and
    [UInt32]$liveReport.replay.maximumCaptureFramesPerSecond -eq 5 -and
    [UInt64]$liveReport.replay.writtenDemodObservations -eq 0 -and
    [UInt64]$liveReport.replay.droppedDemodObservations -eq 0 -and
    [UInt64]$liveReport.replay.fileBytes -eq [UInt64]$replayIdentity.size -and
    (Get-FullPath -Path ([string]$liveReport.replay.path)).Equals($replayIdentity.path, [StringComparison]::OrdinalIgnoreCase)) `
    -Message 'Live Decoder Replay is incomplete, dropped, or path-inconsistent'
Require-Condition -Condition ([bool]$offlineReport.replay.enabled -and [bool]$offlineReport.replay.offlineMode -and
    [bool]$offlineReport.replay.evidenceValid -and [bool]$offlineReport.replay.finalized -and
    [UInt64]$offlineReport.replay.fileBytes -eq [UInt64]$replayIdentity.size -and
    [UInt64]$offlineReport.replay.offlineCaptureFrames -gt 0 -and
    [UInt64]$offlineReport.replay.offlineCaptureFrames -eq [UInt64]$offlineReport.replay.offlineDemodResults -and
    [UInt64]$offlineReport.replay.offlineObservationMismatches -eq 0 -and
    (Get-FullPath -Path ([string]$offlineReport.replay.path)).Equals($replayIdentity.path, [StringComparison]::OrdinalIgnoreCase)) `
    -Message 'Offline Decoder did not deterministically consume the sealed production Replay'

Require-Condition -Condition ([string]$encoderReport.sessionId -ceq [string]$liveReport.sessionId -and
    [string]$liveReport.sessionId -ceq [string]$offlineReport.sessionId -and
    [string]$encoderReport.sessionTag -ceq [string]$liveReport.sessionTag -and
    [string]$liveReport.sessionTag -ceq [string]$offlineReport.sessionTag -and
    [string]$encoderReport.wholeFileDigest -ceq [string]$liveReport.wholeFileDigest -and
    [string]$liveReport.wholeFileDigest -ceq [string]$offlineReport.wholeFileDigest -and
    [string]$liveReport.wholeFileDigest -cmatch '^[0-9a-f]{64}$') `
    -Message 'Computer B, live Decoder, and offline Decoder wire identity mismatch'

$liveOutputPath = Get-FullPath -Path ([string]$liveReport.outputPath)
$offlineOutputPath = Get-FullPath -Path ([string]$offlineReport.outputPath)
Require-Condition -Condition (Test-IsWithin -Path $liveOutputPath -Root (Join-Path $resolvedRoot 'output')) `
    -Message 'Live published output escaped its expected directory'
Require-Condition -Condition (Test-IsWithin -Path $offlineOutputPath -Root (Join-Path $offlineRoot 'output')) `
    -Message 'Offline published output escaped its expected directory'
$liveOutputIdentity = Get-PBFileIdentity -Path $liveOutputPath
$offlineOutputIdentity = Get-PBFileIdentity -Path $offlineOutputPath
Require-Condition -Condition ([UInt64]$liveOutputIdentity.size -eq [UInt64]$sourceIdentity.size -and
    [UInt64]$offlineOutputIdentity.size -eq [UInt64]$sourceIdentity.size -and
    [string]$liveOutputIdentity.sha256 -ceq $ExpectedSourceSha256 -and
    [string]$offlineOutputIdentity.sha256 -ceq $ExpectedSourceSha256) `
    -Message 'Source/live/offline external length or SHA-256 mismatch'
foreach ($directory in @((Join-Path $resolvedRoot 'output'), (Join-Path $offlineRoot 'output')))
{
    $publishedFiles = @(Get-ChildItem -LiteralPath $directory -Force)
    Require-Condition -Condition ($publishedFiles.Count -eq 1 -and -not $publishedFiles[0].PSIsContainer -and
        $publishedFiles[0].Extension -cne '.part') -Message "Output directory contains an unexpected or partial artifact: $directory"
}

$encoderJournal = Read-RunJournal -Path $encoderJournalPath -Role 'Encoder' -RunId $ComputerBRunId `
    -TerminalState 'Stopped' -ExpectedSamples ([UInt64]$encoderReport.evidence.journalSamples) `
    -ExpectedBytes ([UInt64]$encoderReport.evidence.journalBytes)
$liveJournal = Read-RunJournal -Path $liveJournalPath -Role 'Decoder' -RunId $ComputerARunId `
    -TerminalState 'Completed' -ExpectedSamples ([UInt64]$liveReport.evidence.journalSamples) `
    -ExpectedBytes ([UInt64]$liveReport.evidence.journalBytes)
$offlineJournal = Read-RunJournal -Path $offlineJournalPath -Role 'Decoder' -RunId $ComputerARunId `
    -TerminalState 'Completed' -ExpectedSamples ([UInt64]$offlineReport.evidence.journalSamples) `
    -ExpectedBytes ([UInt64]$offlineReport.evidence.journalBytes)
$liveExitCodeIdentity = Require-ExactExitCodeZero -Path $liveExitCodePath
$offlineExitCodeIdentity = Require-ExactExitCodeZero -Path $offlineExitCodePath

$artifacts = [ordered]@{
    kitManifest = Get-PBFileIdentity -Path $KitManifestPath
    source = $sourceIdentity
    routeDecision = Get-PBFileIdentity -Path $routeDecisionPath
    encoderReport = Get-PBFileIdentity -Path $encoderReportPath
    encoderJournal = $encoderJournal.identity
    liveDecoderReport = Get-PBFileIdentity -Path $liveReportPath
    liveDecoderJournal = $liveJournal.identity
    liveDecoderExitCode = $liveExitCodeIdentity
    replay = $replayIdentity
    liveOutput = $liveOutputIdentity
    offlineDecoderReport = Get-PBFileIdentity -Path $offlineReportPath
    offlineDecoderJournal = $offlineJournal.identity
    offlineDecoderExitCode = $offlineExitCodeIdentity
    offlineOutput = $offlineOutputIdentity
}
$result = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21SingleRunAcceptance.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    verified = $true
    step21Status = 'DONE'
    acceptanceContract = 'UserAuthorizedSingleCompleteFilePixelRecovery'
    computerARunId = $ComputerARunId
    computerBRunId = $ComputerBRunId
    evidenceRunIdsEqual = $ComputerARunId -ceq $ComputerBRunId
    evidenceRunIdCorrelation = 'Distinct operator evidence RunIds are retained; exact wire SessionId, SessionTag, WholeFileDigest, profile, runtime commit, and source identity bind both sides.'
    gitCommit = $ExpectedGitCommit
    identity = [ordered]@{
        sessionId = [string]$liveReport.sessionId
        sessionTag = [string]$liveReport.sessionTag
        wholeFileDigest = [string]$liveReport.wholeFileDigest
        sourceBytes = [UInt64]$sourceIdentity.size
        sourceSha256 = [string]$sourceIdentity.sha256
    }
    sender = [ordered]@{
        state = [string]$encoderReport.state
        configuredLogicalVisualFps = [UInt32]$encoderReport.configuredLogicalVisualFps
        generatedVisualFramesPerSecond = [double]$encoderReport.generatedVisualFramesPerSecond
        logicalDwellViolationCount = [UInt64]$encoderReport.logicalDwellViolationCount
        sourceStable = [bool]$encoderReport.sourceStable
        singleMonitorFullscreen = [bool]$encoderReport.dataWindow.singleMonitorFullscreen
        journalSamples = [UInt64]$encoderJournal.samples
    }
    liveReceiver = [ordered]@{
        state = [string]$liveReport.state
        backend = [string]$liveReport.actualBackend
        uniqueVisualFps = [double]$liveReport.uniqueVisualFps
        acceptedLocatorSamples = [UInt64]$liveReport.observedLocatorGeometry.samples
        acceptedTransportBlocks = [UInt64]$liveReport.acceptedTransportBlocks
        outerUniqueSymbols = [UInt64]$liveReport.outerAdmission.uniqueSymbols
        fecFailures = [UInt64]$liveReport.fecFailures
        crcFailures = [UInt64]$liveReport.crcFailures
        wholeFileDigestVerified = [bool]$liveReport.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$liveReport.finalPublishSucceeded
        recoveryRuntimeMilliseconds = [UInt64]$liveReport.recoveryRuntimeMilliseconds
    }
    replay = [ordered]@{
        bytes = [UInt64]$replayIdentity.size
        sha256 = [string]$replayIdentity.sha256
        writtenFrames = [UInt64]$liveReport.replay.writtenFrames
        droppedFrames = [UInt64]$liveReport.replay.droppedFrames
        evidenceValid = [bool]$liveReport.replay.evidenceValid
        finalized = [bool]$liveReport.replay.finalized
    }
    offlineReceiver = [ordered]@{
        state = [string]$offlineReport.state
        captureFrames = [UInt64]$offlineReport.replay.offlineCaptureFrames
        demodResults = [UInt64]$offlineReport.replay.offlineDemodResults
        observationMismatches = [UInt64]$offlineReport.replay.offlineObservationMismatches
        acceptedTransportBlocks = [UInt64]$offlineReport.acceptedTransportBlocks
        outerUniqueSymbols = [UInt64]$offlineReport.outerAdmission.uniqueSymbols
        fecFailures = [UInt64]$offlineReport.fecFailures
        crcFailures = [UInt64]$offlineReport.crcFailures
        wholeFileDigestVerified = [bool]$offlineReport.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$offlineReport.finalPublishSucceeded
        recoveryRuntimeMilliseconds = [UInt64]$offlineReport.recoveryRuntimeMilliseconds
    }
    truthBoundary = [ordered]@{
        actualComputerBEncoderToComputerARightScreenPixels = $true
        decoderUsedOnlyCapturedPixels = $true
        liveAndOfflineReceiverFileRecovery = $true
        crossBrandMatrixWaivedByUser = $true
        crossModeMatrixWaivedByUser = $true
        crossBrandCoverageClaimed = $false
        crossModeCoverageClaimed = $false
        formalProviderGenericMatrixAccepted = $false
        arbitraryGeometryCertified = $false
        certifiedRemoteVisualProfile = $false
    }
    artifacts = $artifacts
}
$outputIdentity = Write-PBCreateOnlyJson -Path $resolvedOutput -Value $result -Depth 24
$result.outputIdentity = $outputIdentity
$result | ConvertTo-Json -Depth 24
