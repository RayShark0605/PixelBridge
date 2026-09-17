#include "decoder_activity.h"

#include <algorithm>

namespace pbapp
{

DecoderActivity DecoderActivityTracker::Observe(const DecoderSnapshot& snapshot, const std::uint64_t elapsedMilliseconds)
{
    const bool reset = !initialized_ || snapshot.runGeneration != previous_.runGeneration || elapsedMilliseconds < lastTime_;
    const auto received = (std::max)(snapshot.estimatedReceivedRawBytes, snapshot.verifiedRawBytes);
    if (reset)
    {
        lastSizeGrowth_ = lastSymbol_ = elapsedMilliseconds;
        resourceWaitSeen_ = repeatedSymbolSeen_ = false;
    }
    else
    {
        if (received > (std::max)(previous_.estimatedReceivedRawBytes, previous_.verifiedRawBytes))
        {
            lastSizeGrowth_ = elapsedMilliseconds;
        }
        if (snapshot.outerUniqueSymbols > previous_.outerUniqueSymbols || snapshot.verifiedSegmentCount > previous_.verifiedSegmentCount)
        {
            lastSymbol_ = elapsedMilliseconds;
        }
        if (snapshot.outerDeferredResourceBusyCount > previous_.outerDeferredResourceBusyCount ||
            snapshot.outerResourceRejections > previous_.outerResourceRejections)
        {
            lastResourceWait_ = elapsedMilliseconds;
            resourceWaitSeen_ = true;
        }
        if (snapshot.outerIdenticalDuplicateSymbols > previous_.outerIdenticalDuplicateSymbols ||
            snapshot.outerAlreadyCompletedSymbols > previous_.outerAlreadyCompletedSymbols)
        {
            lastRepeatedSymbol_ = elapsedMilliseconds;
            repeatedSymbolSeen_ = true;
        }
    }
    initialized_ = true;
    lastTime_ = elapsedMilliseconds;
    previous_ = snapshot;
    DecoderActivity result;
    result.noSizeGrowthMilliseconds = elapsedMilliseconds - lastSizeGrowth_;
    result.noNewSymbolMilliseconds = elapsedMilliseconds - lastSymbol_;
    switch (snapshot.state)
    {
    case DecoderState::Idle: return result;
    case DecoderState::Failed: result.code = "Failed"; return result;
    case DecoderState::Stopped: result.code = "Stopped"; return result;
    case DecoderState::Stopping: result.code = "Stopping"; return result;
    case DecoderState::Completed:
        result.code = snapshot.wholeFileDigestVerified && snapshot.finalPublishSucceeded && snapshot.finalReopenVerified.value_or(false) ? "Completed" : "CompletionUnverified";
        return result;
    case DecoderState::AwaitingLargeOutputConfirmation: result.code = "ConfirmationRequired"; return result;
    default: break;
    }
    if (!snapshot.activeRecoveryOperation.empty())
    {
        result.code = snapshot.activeRecoveryOperation;
        result.normalWait = result.code == "VerifySegment" || result.code == "WriteSegmentAndCheckpoint" ||
            result.code == "VerifyAndPublishFile" || result.code == "CheckpointResume";
        return result;
    }
    if (snapshot.state == DecoderState::Publishing)
    {
        result.code = "Finalizing";
        result.normalWait = true;
        return result;
    }
    if (snapshot.captureStallActive)
    {
        result.code = "CaptureInterrupted";
    }
    else if (snapshot.visualStallActive)
    {
        result.code = "NoFreshVisualData";
    }
    else if (snapshot.state == DecoderState::WaitingForBootstrap)
    {
        result.code = "WaitingForPicture";
    }
    else if (snapshot.state == DecoderState::ReceivingControl || !snapshot.descriptorKnown)
    {
        result.code = "ReceivingDescriptors";
    }
    else if (resourceWaitSeen_ && elapsedMilliseconds - lastResourceWait_ < 5000)
    {
        result.code = "ResourceBackpressure";
    }
    else if (result.noNewSymbolMilliseconds >= 30000)
    {
        result.code = "NoUsefulProgress";
    }
    else if (result.noSizeGrowthMilliseconds < 5000)
    {
        result.code = "Receiving";
    }
    else if (result.noNewSymbolMilliseconds < 5000)
    {
        result.code = "CollectingRepairSymbols";
        result.normalWait = true;
    }
    else if (repeatedSymbolSeen_ && elapsedMilliseconds - lastRepeatedSymbol_ < 5000)
    {
        result.code = "WaitingForCarousel";
        result.normalWait = true;
    }
    else
    {
        result.code = "WaitingForUsefulData";
    }
    return result;
}

} // namespace pbapp
