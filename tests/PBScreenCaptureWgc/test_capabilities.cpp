#include "session_capabilities.h"

#include <catch2/catch_test_macros.hpp>
#include <winrt/Windows.Foundation.h>

using namespace pbscreencapturewgc;
using namespace pbscreencapturewgc::detail;
using namespace winrt::Windows::Graphics::Capture;

namespace
{

struct ProbeState
{
    bool failSet = false;
    bool ignoreSet = false;
    bool cursor = true;
    bool border = true;
    int cursorSets = 0;
    int borderSets = 0;
    int intervalSets = 0;
    winrt::Windows::Foundation::TimeSpan interval{};
};

struct BaselineSession : winrt::implements<BaselineSession, IGraphicsCaptureSession>
{
    void StartCapture()
    {
    }
};

struct ModernSession : winrt::implements<ModernSession, IGraphicsCaptureSession, IGraphicsCaptureSession2, IGraphicsCaptureSession3, IGraphicsCaptureSession5>
{
    explicit ModernSession(std::shared_ptr<ProbeState> state) : state_(std::move(state))
    {
    }
    void StartCapture()
    {
    }
    bool IsCursorCaptureEnabled() const
    {
        return state_->cursor;
    }
    void IsCursorCaptureEnabled(const bool enabled)
    {
        state_->cursorSets++;
        CheckSet();
        if (!state_->ignoreSet)
        {
            state_->cursor = enabled;
        }
    }
    bool IsBorderRequired() const
    {
        return state_->border;
    }
    void IsBorderRequired(const bool required)
    {
        state_->borderSets++;
        CheckSet();
        if (!state_->ignoreSet)
        {
            state_->border = required;
        }
    }
    winrt::Windows::Foundation::TimeSpan MinUpdateInterval() const
    {
        return state_->interval;
    }
    void MinUpdateInterval(const winrt::Windows::Foundation::TimeSpan interval)
    {
        state_->intervalSets++;
        CheckSet();
        if (!state_->ignoreSet)
        {
            state_->interval = interval;
        }
    }
    void CheckSet() const
    {
        if (state_->failSet)
        {
            winrt::throw_hresult(E_ACCESSDENIED);
        }
    }
    std::shared_ptr<ProbeState> state_;
};

}

TEST_CASE("WGC optional controls use interface capabilities, not OS version assumptions")
{
    const auto baseline = winrt::make<BaselineSession>().as<GraphicsCaptureSession>();
    WgcCaptureConfig config;
    config.minUpdateInterval100ns = 12345;
    config.requestBorderless = true;
    CaptureCapabilities capabilities;
    ProbeCursorAndCadence(baseline, config, capabilities);
    REQUIRE_FALSE(capabilities.cursorDisableAvailable);
    REQUIRE_FALSE(capabilities.cursorExcluded);
    REQUIRE_FALSE(capabilities.minUpdateIntervalAvailable);
    REQUIRE_FALSE(capabilities.minUpdateIntervalApplied);
    REQUIRE_FALSE(capabilities.borderlessAvailable);
    ApplyBorderlessPermission(baseline, false, capabilities);
    REQUIRE_FALSE(capabilities.borderlessSettingApplied);
}

TEST_CASE("WGC optional setter failures and denied borderless permission preserve honest capabilities")
{
    for (const bool failure : {false, true})
    {
        const auto state = std::make_shared<ProbeState>();
        state->failSet = failure;
        const auto session = winrt::make<ModernSession>(state).as<GraphicsCaptureSession>();
        WgcCaptureConfig config;
        config.minUpdateInterval100ns = 12345;
        CaptureCapabilities capabilities;
        ProbeCursorAndCadence(session, config, capabilities);
        REQUIRE(capabilities.cursorDisableAvailable);
        REQUIRE(capabilities.minUpdateIntervalAvailable);
        REQUIRE(capabilities.borderlessAvailable);
        REQUIRE(capabilities.cursorExcluded == !failure);
        REQUIRE(capabilities.minUpdateIntervalApplied == !failure);
        REQUIRE(state->cursorSets == 1);
        REQUIRE(state->intervalSets == 1);
        if (failure)
        {
            REQUIRE(capabilities.cursorNativeError == E_ACCESSDENIED);
            REQUIRE(capabilities.minUpdateIntervalNativeError == E_ACCESSDENIED);
        }
        else
        {
            REQUIRE(capabilities.actualMinUpdateInterval100ns == 12345);
        }
        ApplyBorderlessPermission(session, false, capabilities);
        REQUIRE_FALSE(capabilities.borderlessAccessGranted);
        REQUIRE_FALSE(capabilities.borderlessSettingApplied);
        REQUIRE(state->borderSets == 0);
        ApplyBorderlessPermission(session, true, capabilities);
        REQUIRE(capabilities.borderlessAccessGranted);
        REQUIRE(capabilities.borderlessSettingApplied == !failure);
        REQUIRE(state->borderSets == 1);
        if (failure)
        {
            REQUIRE(capabilities.borderlessNativeError == E_ACCESSDENIED);
        }
    }
}

TEST_CASE("WGC ignored settings do not claim cursor exclusion and cadence is not applied unless requested")
{
    const auto state = std::make_shared<ProbeState>();
    state->ignoreSet = true;
    const auto session = winrt::make<ModernSession>(state).as<GraphicsCaptureSession>();
    const WgcCaptureConfig config;
    CaptureCapabilities capabilities;
    ProbeCursorAndCadence(session, config, capabilities);
    REQUIRE(capabilities.cursorDisableAvailable);
    REQUIRE_FALSE(capabilities.cursorExcluded);
    REQUIRE_FALSE(capabilities.minUpdateIntervalApplied);
    REQUIRE(state->intervalSets == 0);
    ApplyBorderlessPermission(session, true, capabilities);
    REQUIRE_FALSE(capabilities.borderlessSettingApplied);
    WgcCaptureConfig requested;
    requested.minUpdateInterval100ns = 12345;
    ProbeCursorAndCadence(session, requested, capabilities);
    REQUIRE(state->intervalSets == 1);
    REQUIRE(capabilities.minUpdateIntervalAvailable);
    REQUIRE_FALSE(capabilities.minUpdateIntervalApplied);
    REQUIRE(capabilities.actualMinUpdateInterval100ns == 0);
}
