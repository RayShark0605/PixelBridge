#include "local_desktop_runtime.h"
#include <iostream>

int main()
{
    pbapp::UnifiedTemporalStripingProbeSnapshot snapshot;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedTemporalStriping(snapshot);
    if (!status)
    {
        std::cerr << status.message << '\n';
        return 1;
    }
    std::cout << "{\"schema\":\"PixelBridge.ControlPhase.StripingCheck.1\",\"headlessOnly\":true,\"pixelObservations\":0,\"sourceBytes\":524288,\"configuredWindowSize\":"
        << snapshot.configuredWindowSize << ",\"passZeroLogicalFrames\":" << snapshot.passZeroLogicalFrames
        << ",\"peakResidentEncodedSegmentCount\":" << snapshot.peakResidentEncodedSegmentCount
        << ",\"peakResidentEncodedSegmentBytes\":" << snapshot.peakResidentEncodedSegmentBytes
        << ",\"receiverDeferredResourceBusyCount\":" << snapshot.receiverDeferredResourceBusyCount
        << ",\"fieldGoodput\":null}\n";
    return snapshot.configuredWindowSize == 8 && snapshot.passZeroLogicalFrames < 1000 &&
        snapshot.peakResidentEncodedSegmentCount <= 8 && snapshot.peakResidentEncodedSegmentBytes <= 524288 &&
        snapshot.receiverDeferredResourceBusyCount == 0 ? 0 : 1;
}
