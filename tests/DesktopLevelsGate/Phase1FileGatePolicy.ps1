#Requires -Version 7.2

function Get-Phase1FileGatePolicy([UInt32]$SoakSeconds, [string]$NativeMode)
{
    if ([string]::IsNullOrWhiteSpace($NativeMode))
    {
        throw 'Phase 1 file Gate mode is required'
    }
    $normalizedMode = $NativeMode.ToLowerInvariant()
    if ($normalizedMode -notin @('release','asan'))
    {
        throw "Unsupported Phase 1 file Gate mode: $NativeMode"
    }
    if ($SoakSeconds -ne 0 -and ($SoakSeconds -lt 300 -or $SoakSeconds -gt 1680))
    {
        throw 'Phase 1 long soak must be disabled or run for at least 300 seconds, with room for the bounded transfer deadline'
    }
    $receiverTimeoutSeconds = if ($SoakSeconds -eq 0) { 90 } else { [UInt32]($SoakSeconds + 120) }
    $regularPostPublishObservationSeconds = [UInt32]15
    return [pscustomobject]@{
        NativeMode=$normalizedMode
        PerformanceCertification=$normalizedMode -ceq 'release'
        SoakSeconds=$SoakSeconds
        RegularPostPublishObservationSeconds=$regularPostPublishObservationSeconds
        RequiredPostPublishObservationSeconds=if ($SoakSeconds -eq 0) { $regularPostPublishObservationSeconds } else { $SoakSeconds }
        ReceiverReadySeconds=[UInt32]15
        ReceiverTimeoutSeconds=$receiverTimeoutSeconds
        ReceiverDeadlineSeconds=[UInt32]($receiverTimeoutSeconds + 20)
        SenderMaximumSeconds=[UInt32]($receiverTimeoutSeconds + 30)
        RestartAfterUniqueFrames=[UInt32]30
        ResizeAfterUniqueFrames=[UInt32]12
        MinimumUniqueVisualFps=if ($normalizedMode -ceq 'release') { 55.0 } else { 0.0 }
        MaximumUniqueVisualFps=65.0
        MinimumUniqueVisualCadenceIntervals=[UInt64]60
        MaximumEndToEndUniqueVisualFps=65.0
        MinimumCaptureDeliveryRatio=if ($normalizedMode -ceq 'release') { 0.90 } else { 0.0 }
        MaximumGpuTimingUnavailableFraction=0.02
        MaximumFrameLeaseHighWater=[UInt32]6
        MaximumConsumerPendingHighWater=[UInt32]4
        MaximumResultQueueHighWater=[UInt32]128
        MaximumForeignSessionErasedFrames=[UInt64]16
        MaximumRetryableUnknownSessionControlDrops=[UInt64]16
        MaximumUnboundSessionVisualFrames=[UInt64]128
        ExpectedCaptureQueuedFrameLimit=[UInt32]4
        ExpectedRoiTextureCount=[UInt32]4
        ExpectedConsumerSlotCount=[UInt32]4
        ExpectedResultQueueCapacity=[UInt32]128
        ExpectedSourceBytes=[UInt64](8 * 1024 * 1024)
        ResourceSampleMilliseconds=[UInt32]1000
        SoakWarmupSeconds=[UInt32]30
        SoakComparisonWindowSeconds=[UInt32]30
        MaximumPrivateMedianDriftBytes=[Int64](32 * 1024 * 1024)
        MaximumPrivateHighWaterIncreaseBytes=[Int64](64 * 1024 * 1024)
        MaximumHandleMedianDrift=[Int64]4
        MaximumHandleHighWaterIncrease=[Int64]16
    }
}

function Assert-FinitePositive([object]$Value, [string]$Name)
{
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le 0)
    {
        throw "$Name must be finite and positive"
    }
}

function Assert-NearlyEqual([object]$Actual, [double]$Expected, [string]$Name)
{
    $actualNumber = [double]$Actual
    if ([double]::IsNaN($actualNumber) -or [double]::IsInfinity($actualNumber) -or
        [double]::IsNaN($Expected) -or [double]::IsInfinity($Expected))
    {
        throw "$Name must be finite"
    }
    $tolerance = [Math]::Max(1.0e-12, [Math]::Abs($Expected) * 1.0e-9)
    if ([Math]::Abs($actualNumber - $Expected) -gt $tolerance)
    {
        throw "$Name disagrees with its authoritative numerator/denominator"
    }
}

function Assert-Phase1FileGateRecords(
    [object]$Sender,
    [object]$Receiver,
    [object]$External,
    [object]$Resources,
    [string]$Profile,
    [string]$Backend,
    [object]$Policy)
{
    if ($Profile -notin @('desktop-levels-2x2','shape-chroma') -or $Backend -notin @('wgc','dxgi'))
    {
        throw 'Unknown Phase 1 file Gate profile or backend'
    }
    if ($Sender.event -cne 'phase1-sender-final' -or $Sender.status -cne 'pass' -or $Sender.profile -cne $Profile)
    {
        throw 'Sender did not publish the expected authoritative final record'
    }
    if ([UInt64]$Sender.sourceBytes -ne $Policy.ExpectedSourceBytes -or
        [UInt64]$Sender.encodedBytes -ne $Policy.ExpectedSourceBytes -or
        [UInt64]$Sender.submittedVisualFrames -ne
            ([UInt64]$Sender.dataFrames + [UInt64]$Sender.controlFrames + [UInt64]$Sender.injectedTornFrames) -or
        [UInt64]$Sender.dataFrames -eq 0 -or [UInt64]$Sender.controlFrames -eq 0 -or [UInt64]$Sender.injectedTornFrames -eq 0)
    {
        throw 'Sender source size or finite frame schedule is inconsistent'
    }
    if (-not [bool]$Sender.candidateContractSatisfied -or -not [bool]$Sender.tearingDisabled -or
        -not [bool]$Sender.latencyWaitable -or [UInt32]$Sender.maximumFrameLatency -ne 1 -or
        -not [bool]$Sender.cleanShutdown -or [bool]$Sender.pendingAtShutdown -or [bool]$Sender.inFlightAtShutdown -or
        [UInt64]$Sender.successfulPresents -lt 60 -or [UInt64]$Sender.presentCalls -ne [UInt64]$Sender.successfulPresents)
    {
        throw 'Stable flip/vsync presentation contract was not maintained'
    }
    if ([UInt64]$Sender.presentationEpoch -lt 2 -or [UInt64]$Sender.swapChainGeneration -lt 1 -or
        [UInt64]$Sender.bufferGeneration -lt 2)
    {
        throw 'The real DataWindow resize/buffer-generation/presentation-epoch recovery did not complete'
    }
    if ($Receiver.event -cne 'phase1-receiver-final' -or $Receiver.status -cne 'pass' -or
        $Receiver.profile -cne $Profile -or $Receiver.backend -cne $Backend)
    {
        throw 'Receiver did not publish the expected authoritative final record'
    }
    if ([UInt64]$Receiver.sourceBytes -ne $Policy.ExpectedSourceBytes -or
        [UInt64]$Receiver.verifiedEncodedBytes -ne $Policy.ExpectedSourceBytes -or
        [UInt64]$Receiver.storedSegments -ne 1 -or -not [bool]$Receiver.wholeFileDigestVerified -or
        -not [bool]$Receiver.publishedFinalDigestVerified -or
        [string]$Receiver.wholeFileDigest -cne [string]$Sender.wholeFileDigest -or
        [string]$Receiver.sessionTag -cne [string]$Sender.sessionTag)
    {
        throw 'Authoritative Receiver/Storage/WholeFileDigest evidence is inconsistent'
    }
    Assert-FinitePositive $Receiver.verifiedEncodedGoodputBytesPerSecond 'VerifiedEncodedGoodput'
    Assert-FinitePositive $Receiver.verifiedEncodedGoodputMiBPerSecond 'VerifiedEncodedGoodputMiBPerSecond'
    Assert-NearlyEqual $Receiver.verifiedEncodedGoodputMiBPerSecond `
        ([double]$Receiver.verifiedEncodedGoodputBytesPerSecond / (1024.0 * 1024.0)) 'VerifiedEncodedGoodputMiBPerSecond'
    $fer = [double]$Receiver.postFecFer
    $evaluatedAccounting = [decimal]$Receiver.verifiedDataFrames + [decimal]$Receiver.postFecFailedFrames
    $transportAccounting = [decimal]$Receiver.receiverDataBlocks + [decimal]$Receiver.orphanQuotaDrops
    if ([double]::IsNaN($fer) -or [double]::IsInfinity($fer) -or $fer -lt 0 -or $fer -gt 1 -or
        [UInt64]$Receiver.evaluatedDataFrames -eq 0 -or [UInt64]$Receiver.verifiedDataFrames -eq 0 -or
        $evaluatedAccounting -gt [decimal][UInt64]::MaxValue -or
        [decimal]$Receiver.evaluatedDataFrames -ne $evaluatedAccounting -or
        [UInt64]$Receiver.acceptedTransportBlocks -eq 0 -or
        $transportAccounting -gt [decimal][UInt64]::MaxValue -or
        [decimal]$Receiver.acceptedTransportBlocks -ne $transportAccounting -or
        [UInt64]$Receiver.resourcePolicyRejectedCount -ne [UInt64]$Receiver.orphanQuotaDrops -or
        [UInt64]$Receiver.orphanResourceExhaustedCount -ne 0 -or
        [UInt64]$Receiver.orphanConflictRejectionCount -ne 0 -or [UInt64]$Receiver.orphanCachedBlockCount -ne 0)
    {
        throw 'Post-FEC FER or Transport accounting is invalid'
    }
    Assert-NearlyEqual $fer ([double]$Receiver.postFecFailedFrames / [double]$Receiver.evaluatedDataFrames) 'PostFecFER'
    if ([UInt64]$Receiver.foreignSessionErasedFrames -gt $Policy.MaximumForeignSessionErasedFrames -or
        [UInt64]$Receiver.retryableUnknownSessionControlDrops -gt $Policy.MaximumRetryableUnknownSessionControlDrops -or
        [UInt64]$Receiver.unboundSessionVisualFrames -gt $Policy.MaximumUnboundSessionVisualFrames)
    {
        throw 'Foreign-session, pre-rebind Control, or unbound visual erasures exceeded the Gate bound'
    }
    $uniqueVisualFps = [double]$Receiver.uniqueVisualFps
    $endToEndUniqueVisualFps = [double]$Receiver.endToEndUniqueVisualFps
    if ([double]::IsNaN($uniqueVisualFps) -or [double]::IsInfinity($uniqueVisualFps) -or
        $uniqueVisualFps -le 0 -or
        ([bool]$Policy.PerformanceCertification -and $uniqueVisualFps -lt $Policy.MinimumUniqueVisualFps) -or
        $uniqueVisualFps -gt $Policy.MaximumUniqueVisualFps -or
        [double]::IsNaN($endToEndUniqueVisualFps) -or [double]::IsInfinity($endToEndUniqueVisualFps) -or
        $endToEndUniqueVisualFps -le 0 -or $endToEndUniqueVisualFps -gt $Policy.MaximumEndToEndUniqueVisualFps -or
        [UInt64]$Receiver.uniqueVisualFrames -lt 120 -or [UInt64]$Receiver.reorderedVisualFrames -ne 0 -or
        [UInt64]$Receiver.uniqueVisualCadenceIntervals -lt $Policy.MinimumUniqueVisualCadenceIntervals -or
        [UInt64]$Receiver.uniqueVisualCadenceTime100ns -eq 0 -or
        [UInt64]$Receiver.uniqueVisualGapEvents -eq 0 -or [UInt64]$Receiver.skippedVisualSequences -eq 0)
    {
        throw 'Steady/end-to-end UniqueVisualFPS or visual identity evidence is outside the 1080p60 Gate contract'
    }
    Assert-NearlyEqual $uniqueVisualFps `
        ([double]$Receiver.uniqueVisualCadenceIntervals * 10000000.0 / [double]$Receiver.uniqueVisualCadenceTime100ns) `
        'UniqueVisualFPS'
    $visualTransitions = [decimal]$Receiver.uniqueVisualCadenceIntervals + [decimal]$Receiver.uniqueVisualGapEvents +
        [decimal]$Receiver.visualCaptureEpochBoundaries
    if ($visualTransitions -ne ([decimal]$Receiver.uniqueVisualFrames - 1) -or
        [UInt64]$Receiver.skippedVisualSequences -lt [UInt64]$Receiver.uniqueVisualGapEvents)
    {
        throw 'Unique visual transition counters are internally inconsistent'
    }
    if ([UInt64]$Receiver.captureSessions -ne 2 -or [UInt64]$Receiver.captureRestarts -ne 1 -or
        [UInt64]$Receiver.inPlaceCaptureRecreates -lt 1 -or [UInt64]$Receiver.receiverCaptureEpochResets -ne 2 -or
        [UInt64]$Receiver.visualCaptureEpochBoundaries -ne [UInt64]$Receiver.receiverCaptureEpochResets -or
        [UInt64]$Receiver.captureDomainStarts -lt 3 -or [UInt64]$Receiver.captureDomainInvalidations -lt 3 -or
        [UInt64]$Receiver.captureArrivals -eq 0 -or [UInt64]$Receiver.captureDelivered -eq 0 -or
        [UInt64]$Receiver.captureDelivered -gt [UInt64]$Receiver.captureArrivals -or
        [decimal]$Receiver.captureDrops + [decimal]$Receiver.captureExpired -gt [decimal]$Receiver.captureArrivals -or
        [UInt32]$Receiver.captureQueuedFrameLimit -ne $Policy.ExpectedCaptureQueuedFrameLimit -or
        [UInt32]$Receiver.roiTextureCount -ne $Policy.ExpectedRoiTextureCount -or
        [UInt32]$Receiver.consumerSlotCount -ne $Policy.ExpectedConsumerSlotCount -or
        [UInt32]$Receiver.resultQueueCapacity -ne $Policy.ExpectedResultQueueCapacity -or
        [UInt64]$Receiver.frameLeaseHighWater -gt $Policy.MaximumFrameLeaseHighWater -or
        [UInt64]$Receiver.consumerPendingHighWater -gt $Policy.MaximumConsumerPendingHighWater -or
        [UInt64]$Receiver.resultQueueHighWater -gt $Policy.MaximumResultQueueHighWater -or
        [UInt64]$Receiver.resultQueueDrops -ne 0 -or [UInt64]$Receiver.staleResultDrops -gt $Policy.MaximumResultQueueHighWater)
    {
        throw 'CaptureEpoch recovery or bounded FrameLease/backlog accounting failed'
    }
    $captureDeliveryRatio = [double]$Receiver.captureDeliveryRatio
    if ([double]::IsNaN($captureDeliveryRatio) -or [double]::IsInfinity($captureDeliveryRatio) -or
        $captureDeliveryRatio -lt [double]$Policy.MinimumCaptureDeliveryRatio -or $captureDeliveryRatio -gt 1)
    {
        throw 'Capture delivery ratio is outside the stable 1080p60 Gate contract'
    }
    Assert-NearlyEqual $captureDeliveryRatio `
        ([double]$Receiver.captureDelivered / [double]$Receiver.captureArrivals) 'CaptureDeliveryRatio'
    if ([UInt64]$Receiver.bootstrapRejectedFrames -eq 0 -or [UInt64]$Receiver.bootstrapMismatchFrames -eq 0 -or
        [UInt64]$Receiver.bootstrapMismatchFrames -gt [UInt64]$Receiver.bootstrapRejectedFrames -or
        [UInt64]$Receiver.controlFrameFailures -ne 0)
    {
        throw 'Injected A/B mixed/torn frames were not erased, or fixed Control decoding failed'
    }
    foreach ($metric in @('roiGpuAverageMilliseconds','demodGpuAverageMilliseconds',
        'bootstrapCpuAverageMilliseconds','postGpuFecCpuAverageMilliseconds','finalVerificationMilliseconds'))
    {
        Assert-FinitePositive $Receiver.$metric $metric
    }
    foreach ($prefix in @('roiGpu','demodGpu'))
    {
        $samples = [UInt64]$Receiver.("${prefix}TimingSamples")
        $unavailable = [UInt64]$Receiver.("${prefix}TimingUnavailable")
        if ($samples -eq 0)
        {
            throw "$prefix has no real timing sample"
        }
        $total = [double]$samples + [double]$unavailable
        if ($total -le 0 -or ([double]$unavailable / $total) -gt $Policy.MaximumGpuTimingUnavailableFraction)
        {
            throw "$prefix unavailable timing ratio exceeds the Gate limit"
        }
    }
    if ([UInt64]$Receiver.bootstrapCpuTimingSamples -eq 0 -or
        [UInt64]$Receiver.postGpuFecCpuTimingSamples -eq 0 -or [UInt64]$Receiver.cpuTimingUnavailable -ne 0)
    {
        throw 'CPU timing evidence is incomplete'
    }
    if (-not [bool]$Receiver.allCaptureShutdownComplete -or [bool]$Receiver.anyDeferredCleanup -or
        -not [bool]$Receiver.allConsumersShutdown -or -not [bool]$Receiver.allDomainsInactive)
    {
        throw 'Capture/demod shutdown retained resources or an active domain'
    }
    $postPublishObservationSeconds = [double]$Receiver.postPublishObservationSeconds
    if ([UInt32]$Receiver.soakSeconds -ne [UInt32]$Policy.SoakSeconds -or
        [UInt32]$Receiver.requiredPostPublishObservationSeconds -ne [UInt32]$Policy.RequiredPostPublishObservationSeconds -or
        [double]::IsNaN($postPublishObservationSeconds) -or [double]::IsInfinity($postPublishObservationSeconds) -or
        $postPublishObservationSeconds -lt [double]$Policy.RequiredPostPublishObservationSeconds -or
        [UInt64]$Receiver.elapsedMilliseconds -lt ([UInt64]$Policy.RequiredPostPublishObservationSeconds * 1000))
    {
        throw 'Receiver did not execute the required bounded post-publication observation duration'
    }
    if (-not [bool]$External.SourceOutputSha256Match -or -not [bool]$External.PartFileAbsent -or
        -not [bool]$External.ResizeApplied -or -not [bool]$External.VisibilityMaintained)
    {
        throw 'Independent file digest, publication, resize, or visibility evidence failed'
    }
    Assert-FinitePositive $Resources.SenderEquivalentCpuCores 'sender equivalent CPU cores'
    Assert-FinitePositive $Resources.ReceiverEquivalentCpuCores 'receiver equivalent CPU cores'
    if ([UInt64]$Resources.Samples -lt 2 -or
        [double]$Resources.SenderEquivalentCpuCores -gt [double]$Resources.LogicalProcessorCount -or
        [double]$Resources.ReceiverEquivalentCpuCores -gt [double]$Resources.LogicalProcessorCount)
    {
        throw 'Process CPU/resource samples are incomplete or invalid'
    }
    if ($Policy.SoakSeconds -eq 0)
    {
        if ([bool]$Resources.LongSoakVerified)
        {
            throw 'A non-soak run was mislabeled as long-soak evidence'
        }
    }
    elseif (-not [bool]$Resources.LongSoakVerified -or
        [Int64]$Resources.SenderPrivateMedianDriftBytes -gt $Policy.MaximumPrivateMedianDriftBytes -or
        [Int64]$Resources.ReceiverPrivateMedianDriftBytes -gt $Policy.MaximumPrivateMedianDriftBytes -or
        [Int64]$Resources.SenderPrivateHighWaterIncreaseBytes -gt $Policy.MaximumPrivateHighWaterIncreaseBytes -or
        [Int64]$Resources.ReceiverPrivateHighWaterIncreaseBytes -gt $Policy.MaximumPrivateHighWaterIncreaseBytes -or
        [Int64]$Resources.SenderHandleMedianDrift -gt $Policy.MaximumHandleMedianDrift -or
        [Int64]$Resources.ReceiverHandleMedianDrift -gt $Policy.MaximumHandleMedianDrift -or
        [Int64]$Resources.SenderHandleHighWaterIncrease -gt $Policy.MaximumHandleHighWaterIncrease -or
        [Int64]$Resources.ReceiverHandleHighWaterIncrease -gt $Policy.MaximumHandleHighWaterIncrease)
    {
        throw 'Long-soak private-memory or handle growth exceeds the bounded leak threshold'
    }
}
