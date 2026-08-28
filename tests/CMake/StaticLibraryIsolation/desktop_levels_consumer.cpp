#include "pbdesktoplevels/reference_channel.h"

int main()
{
    auto channel = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    if (!channel)
    {
        return 1;
    }
    const auto observation = channel.Value().Decode({});
    return observation.modulation.IsAccepted() || observation.evaluation.evaluated ? 2 : 0;
}
