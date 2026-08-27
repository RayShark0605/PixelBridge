#include "pbpresenttiming/present_timing.h"

int main()
{
    pbpresenttiming::PresentTiming timing(1000);
    if (!timing.BeginEpoch(pbpresenttiming::EpochReason::Initial, 0))
    {
        return 1;
    }
    timing.RecordPresent(1, 10, 11, pbpresenttiming::PresentOutcome::Success, 1);
    const auto snapshot = timing.GetSnapshot(100);
    return snapshot.presentCalls == 1 && snapshot.presentCallFps && !snapshot.presentedVisualFps ? 0 : 2;
}
