#Requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'Phase1FileGatePolicy.ps1')

function New-ValidRecords([UInt32]$SoakSeconds, [string]$NativeMode = 'release')
{
    $policy = Get-Phase1FileGatePolicy $SoakSeconds $NativeMode
    $sender = [pscustomobject]@{
        event='phase1-sender-final'; status='pass'; profile='desktop-levels-2x2'; sessionTag='7'
        sourceBytes=[UInt64](8 * 1024 * 1024); encodedBytes=[UInt64](8 * 1024 * 1024); wholeFileDigest='abc'
        submittedVisualFrames=[UInt64]300; dataFrames=[UInt64]270; controlFrames=[UInt64]20; injectedTornFrames=[UInt64]10
        candidateContractSatisfied=$true; tearingDisabled=$true; latencyWaitable=$true; maximumFrameLatency=[UInt32]1
        cleanShutdown=$true; pendingAtShutdown=$false; inFlightAtShutdown=$false
        successfulPresents=[UInt64]300; presentCalls=[UInt64]300; presentationEpoch=[UInt64]3
        swapChainGeneration=[UInt64]1; bufferGeneration=[UInt64]3
    }
    $receiver = [pscustomobject]@{
        event='phase1-receiver-final'; status='pass'; profile='desktop-levels-2x2'; backend='wgc'; sessionTag='7'
        sourceBytes=[UInt64](8 * 1024 * 1024); verifiedEncodedBytes=[UInt64](8 * 1024 * 1024); storedSegments=[UInt64]1
        wholeFileDigest='abc'; wholeFileDigestVerified=$true; publishedFinalDigestVerified=$true
        verifiedEncodedGoodputBytesPerSecond=1000000.0; verifiedEncodedGoodputMiBPerSecond=0.95367431640625
        postFecFer=0.01; evaluatedDataFrames=[UInt64]200; verifiedDataFrames=[UInt64]198; postFecFailedFrames=[UInt64]2
        acceptedTransportBlocks=[UInt64]4000; receiverDataBlocks=[UInt64]3980; orphanQuotaDrops=[UInt64]20
        resourcePolicyRejectedCount=[UInt64]20; orphanResourceExhaustedCount=[UInt64]0
        orphanConflictRejectionCount=[UInt64]0; orphanCachedBlockCount=[UInt64]0
        foreignSessionErasedFrames=[UInt64]1; retryableUnknownSessionControlDrops=[UInt64]1
        unboundSessionVisualFrames=[UInt64]1
        uniqueVisualFrames=[UInt64]300; duplicateVisualFrames=[UInt64]3; reorderedVisualFrames=[UInt64]0
        uniqueVisualFps=62.5; endToEndUniqueVisualFps=52.0; uniqueVisualCadenceIntervals=[UInt64]293
        uniqueVisualCadenceTime100ns=[UInt64]46880000; uniqueVisualGapEvents=[UInt64]4
        skippedVisualSequences=[UInt64]4; visualCaptureEpochBoundaries=[UInt64]2
        captureSessions=[UInt64]2; captureRestarts=[UInt64]1; inPlaceCaptureRecreates=[UInt64]1
        receiverCaptureEpochResets=[UInt64]2; captureDomainStarts=[UInt64]3; captureDomainInvalidations=[UInt64]3
        captureArrivals=[UInt64]400; captureDelivered=[UInt64]380; captureDeliveryRatio=0.95
        captureDrops=[UInt64]10; captureExpired=[UInt64]2
        captureQueuedFrameLimit=[UInt32]4; roiTextureCount=[UInt32]4; consumerSlotCount=[UInt32]4
        resultQueueCapacity=[UInt32]128; frameLeaseHighWater=[UInt64]6
        consumerPendingHighWater=[UInt64]4; resultQueueHighWater=[UInt64]8
        resultQueueDrops=[UInt64]0; staleResultDrops=[UInt64]0; bootstrapRejectedFrames=[UInt64]10
        bootstrapMismatchFrames=[UInt64]4; controlFrameFailures=[UInt64]0
        roiGpuTimingSamples=[UInt64]300; roiGpuTimingUnavailable=[UInt64]0; roiGpuAverageMilliseconds=0.1
        demodGpuTimingSamples=[UInt64]200; demodGpuTimingUnavailable=[UInt64]0; demodGpuAverageMilliseconds=0.2
        bootstrapCpuTimingSamples=[UInt64]350; bootstrapCpuAverageMilliseconds=0.05
        postGpuFecCpuTimingSamples=[UInt64]200; postGpuFecCpuAverageMilliseconds=0.08; cpuTimingUnavailable=[UInt64]0
        finalVerificationMilliseconds=12.0; allCaptureShutdownComplete=$true; anyDeferredCleanup=$false
        allConsumersShutdown=$true; allDomainsInactive=$true; soakSeconds=$SoakSeconds
        requiredPostPublishObservationSeconds=[UInt32]$policy.RequiredPostPublishObservationSeconds
        postPublishObservationSeconds=[double]$policy.RequiredPostPublishObservationSeconds + 0.25
        elapsedMilliseconds=[UInt64]([UInt64]$policy.RequiredPostPublishObservationSeconds * 1000 + 6000)
    }
    $external = [pscustomobject]@{
        SourceOutputSha256Match=$true; PartFileAbsent=$true; ResizeApplied=$true; VisibilityMaintained=$true
    }
    $resources = [pscustomobject]@{
        Samples=[UInt64]$(if ($SoakSeconds -eq 0) { 6 } else { 300 }); LogicalProcessorCount=[UInt32]16
        SenderEquivalentCpuCores=1.25; ReceiverEquivalentCpuCores=0.75; LongSoakVerified=$SoakSeconds -ne 0
        SenderPrivateMedianDriftBytes=[Int64]0; ReceiverPrivateMedianDriftBytes=[Int64]0
        SenderPrivateHighWaterIncreaseBytes=[Int64]0; ReceiverPrivateHighWaterIncreaseBytes=[Int64]0
        SenderHandleMedianDrift=[Int64]0; ReceiverHandleMedianDrift=[Int64]0
        SenderHandleHighWaterIncrease=[Int64]0; ReceiverHandleHighWaterIncrease=[Int64]0
    }
    return [pscustomobject]@{ Policy=$policy; Sender=$sender; Receiver=$receiver; External=$external; Resources=$resources }
}

function Copy-RecordSet([object]$Records)
{
    return ($Records | ConvertTo-Json -Depth 12 | ConvertFrom-Json)
}

function Assert-Rejected([string]$Name, [scriptblock]$Mutation, [string]$NativeMode = 'release')
{
    $records = Copy-RecordSet (New-ValidRecords 0 $NativeMode)
    & $Mutation $records
    $rejected = $false
    try
    {
        Assert-Phase1FileGateRecords $records.Sender $records.Receiver $records.External $records.Resources `
            'desktop-levels-2x2' 'wgc' $records.Policy
    }
    catch
    {
        $rejected = $true
    }
    if (-not $rejected)
    {
        throw "Invalid Phase 1 file evidence was accepted: $Name"
    }
}

$regular = New-ValidRecords 0
Assert-Phase1FileGateRecords $regular.Sender $regular.Receiver $regular.External $regular.Resources `
    'desktop-levels-2x2' 'wgc' $regular.Policy
if ($regular.Policy.ReceiverReadySeconds -eq 0 -or $regular.Policy.ReceiverReadySeconds -gt 30 -or
    $regular.Policy.ReceiverReadySeconds -ge $regular.Policy.ReceiverTimeoutSeconds -or
    $regular.Policy.RegularPostPublishObservationSeconds -ne 15 -or
    $regular.Policy.RequiredPostPublishObservationSeconds -ne 15 -or
    -not [bool]$regular.Policy.PerformanceCertification -or $regular.Policy.NativeMode -cne 'release')
{
    throw 'Receiver readiness wait is not bounded inside the child timeout'
}
$loopTokens = $null
$loopParseErrors = $null
$loopAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'InvokePhase1FileLoop.ps1'), [ref]$loopTokens, [ref]$loopParseErrors)
if ($loopParseErrors.Count -ne 0)
{
    throw 'InvokePhase1FileLoop.ps1 does not parse'
}
$processStarts = @($loopAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Right.Extent.Text -match '^Start-OwnedProcess\s'
}, $true))
$receiverStarts = @($processStarts | Where-Object { $_.Left.VariablePath.UserPath -ieq 'receiver' })
$senderStarts = @($processStarts | Where-Object { $_.Left.VariablePath.UserPath -ieq 'sender' })
if ($receiverStarts.Count -ne 1 -or $senderStarts.Count -ne 1 -or
    $receiverStarts[0].Extent.StartOffset -ge $senderStarts[0].Extent.StartOffset)
{
    throw 'Physical file loop must arm the bounded Receiver before starting the finite Sender preamble'
}
$soak = New-ValidRecords 300
Assert-Phase1FileGateRecords $soak.Sender $soak.Receiver $soak.External $soak.Resources `
    'desktop-levels-2x2' 'wgc' $soak.Policy
if ($soak.Policy.RequiredPostPublishObservationSeconds -ne 300)
{
    throw 'Long-soak post-publication observation requirement was changed'
}

# ASan remains an authoritative safety/correctness run, but instrumentation
# cost must not be misreported as the Release 1080p60 performance baseline.
# All digest, FER, recovery, bounded-resource, timing-presence and accounting
# checks remain active; only the two real-time throughput-health thresholds are
# non-certifying in this mode.
$asan = New-ValidRecords 0 'asan'
$asan.Receiver.uniqueVisualFps = 10.0
$asan.Receiver.uniqueVisualCadenceTime100ns = [UInt64]293000000
$asan.Receiver.endToEndUniqueVisualFps = 5.0
$asan.Receiver.captureDelivered = [UInt64]356
$asan.Receiver.captureDeliveryRatio = 0.89
Assert-Phase1FileGateRecords $asan.Sender $asan.Receiver $asan.External $asan.Resources `
    'desktop-levels-2x2' 'wgc' $asan.Policy
if ([bool]$asan.Policy.PerformanceCertification -or $asan.Policy.NativeMode -cne 'asan')
{
    throw 'ASan file Gate was mislabeled as a Release performance certification'
}

$invalidDurations = 0
foreach ($duration in @([UInt32]1,[UInt32]299,[UInt32]1681))
{
    try
    {
        $null = Get-Phase1FileGatePolicy $duration 'release'
    }
    catch
    {
        $invalidDurations++
    }
}
if ($invalidDurations -ne 3)
{
    throw 'Pseudo-soak or unbounded soak duration was accepted'
}

$invalidModes = 0
foreach ($mode in @('','debug','instrumented'))
{
    try
    {
        $null = Get-Phase1FileGatePolicy 0 $mode
    }
    catch
    {
        $invalidModes++
    }
}
if ($invalidModes -ne 3)
{
    throw 'Unknown or empty file Gate mode was accepted'
}

$mutations = [ordered]@{
    tearing = { param($records) $records.Sender.tearingDisabled = $false }
    senderShutdown = { param($records) $records.Sender.pendingAtShutdown = $true }
    resize = { param($records) $records.Sender.presentationEpoch = 1 }
    resizeBuffers = { param($records) $records.Sender.bufferGeneration = 1 }
    swapChainMissing = { param($records) $records.Sender.swapChainGeneration = 0 }
    digest = { param($records) $records.Receiver.wholeFileDigest = 'different' }
    goodputUnit = { param($records) $records.Receiver.verifiedEncodedGoodputMiBPerSecond = 1.0 }
    fer = { param($records) $records.Receiver.postFecFer = 1.1 }
    ferAccounting = { param($records) $records.Receiver.postFecFer = 0.02 }
    transportAccounting = { param($records) $records.Receiver.receiverDataBlocks = 3979 }
    orphanDropAccounting = { param($records) $records.Receiver.resourcePolicyRejectedCount = 19 }
    orphanExhaustion = { param($records) $records.Receiver.orphanResourceExhaustedCount = 1 }
    orphanConflict = { param($records) $records.Receiver.orphanConflictRejectionCount = 1 }
    orphanResidue = { param($records) $records.Receiver.orphanCachedBlockCount = 1 }
    foreignSessionErasureBound = { param($records) $records.Receiver.foreignSessionErasedFrames = [UInt64]17 }
    retryableUnknownSessionBound = { param($records) $records.Receiver.retryableUnknownSessionControlDrops = [UInt64]17 }
    unboundSessionVisualBound = { param($records) $records.Receiver.unboundSessionVisualFrames = [UInt64]129 }
    uniqueFps = { param($records) $records.Receiver.uniqueVisualFps = 54.9 }
    uniqueFpsAccounting = { param($records) $records.Receiver.uniqueVisualFps = 62.4 }
    endToEndFps = { param($records) $records.Receiver.endToEndUniqueVisualFps = 65.1 }
    cadenceIntervals = { param($records) $records.Receiver.uniqueVisualCadenceIntervals = 59 }
    cadenceTime = { param($records) $records.Receiver.uniqueVisualCadenceTime100ns = 0 }
    visualGap = { param($records) $records.Receiver.uniqueVisualGapEvents = 0 }
    skippedSequence = { param($records) $records.Receiver.skippedVisualSequences = 0 }
    visualTransitions = { param($records) $records.Receiver.uniqueVisualGapEvents = 3 }
    epochBoundary = { param($records) $records.Receiver.visualCaptureEpochBoundaries = 1 }
    recovery = { param($records) $records.Receiver.captureRestarts = 0 }
    captureDelivery = { param($records) $records.Receiver.captureDeliveryRatio = 0.89 }
    captureDeliveryAccounting = { param($records) $records.Receiver.captureDeliveryRatio = 0.96 }
    captureTerminalAccounting = { param($records) $records.Receiver.captureDrops = 399 }
    observationRequirement = { param($records) $records.Receiver.requiredPostPublishObservationSeconds = [UInt32]($records.Receiver.requiredPostPublishObservationSeconds - 1) }
    observationDuration = { param($records) $records.Receiver.postPublishObservationSeconds = [double]$records.Policy.RequiredPostPublishObservationSeconds - 0.001 }
    backlogCapacity = { param($records) $records.Receiver.consumerSlotCount = 5 }
    queueDrop = { param($records) $records.Receiver.resultQueueDrops = 1 }
    torn = { param($records) $records.Receiver.bootstrapMismatchFrames = 0 }
    tornAccounting = { param($records) $records.Receiver.bootstrapMismatchFrames = 11 }
    gpuTiming = { param($records) $records.Receiver.roiGpuTimingUnavailable = 7 }
    shutdown = { param($records) $records.Receiver.allConsumersShutdown = $false }
    independentDigest = { param($records) $records.External.SourceOutputSha256Match = $false }
    cpuCost = { param($records) $records.Resources.ReceiverEquivalentCpuCores = 0 }
}
foreach ($entry in $mutations.GetEnumerator())
{
    Assert-Rejected $entry.Key $entry.Value
}
Assert-Rejected 'asan-still-rejects-digest-failure' { param($records) $records.Receiver.wholeFileDigestVerified = $false } 'asan'
Assert-Rejected 'asan-still-rejects-impossible-fps' { param($records) $records.Receiver.uniqueVisualFps = 65.1 } 'asan'

$leak = Copy-RecordSet (New-ValidRecords 300)
$leak.Resources.ReceiverPrivateMedianDriftBytes = [Int64](33 * 1024 * 1024)
$leakRejected = $false
try
{
    Assert-Phase1FileGateRecords $leak.Sender $leak.Receiver $leak.External $leak.Resources `
        'desktop-levels-2x2' 'wgc' $leak.Policy
}
catch
{
    $leakRejected = $true
}
if (-not $leakRejected)
{
    throw 'Long-soak private-memory drift was accepted'
}

Write-Output "PHASE1_FILE_GATE_POLICY_PASS valid=3 invalid-duration=3 invalid-mode=3 mutations=$($mutations.Count + 2) leak=1 receiver-first=1"
