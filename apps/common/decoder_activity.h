#pragma once

#include "application_model.h"

namespace pbapp
{

struct DecoderActivity
{
    std::string code = "Idle";
    bool normalWait = false;
    std::uint64_t noSizeGrowthMilliseconds = 0;
    std::uint64_t noNewSymbolMilliseconds = 0;
};

// Observation only. Never feeds admission, timeout, progress or file acceptance.
// Monotonic caller time and runGeneration prevent wall-clock/previous-run leaks.
class DecoderActivityTracker
{
public:
    [[nodiscard]] DecoderActivity Observe(const DecoderSnapshot& snapshot, std::uint64_t elapsedMilliseconds);

private:
    DecoderSnapshot previous_;
    bool initialized_ = false;
    std::uint64_t lastTime_ = 0;
    std::uint64_t lastSizeGrowth_ = 0;
    std::uint64_t lastSymbol_ = 0;
    std::uint64_t lastResourceWait_ = 0;
    std::uint64_t lastRepeatedSymbol_ = 0;
    bool resourceWaitSeen_ = false;
    bool repeatedSymbolSeen_ = false;
};

} // namespace pbapp
