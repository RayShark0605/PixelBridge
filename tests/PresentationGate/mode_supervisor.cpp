#include "gate_support.h"
#include "pbprotocol/session_random.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <sstream>
#include <string_view>

namespace presentationgate
{
namespace
{
using namespace pbrenderd3d;

struct DisplaySettings
{
    std::wstring name;
    DEVMODEW mode{};
    bool primary = false;
};

[[nodiscard]] bool SameMode(const DEVMODEW& first, const DEVMODEW& second) noexcept
{
    return first.dmPelsWidth == second.dmPelsWidth && first.dmPelsHeight == second.dmPelsHeight && first.dmBitsPerPel == second.dmBitsPerPel &&
           first.dmDisplayFrequency == second.dmDisplayFrequency && first.dmPosition.x == second.dmPosition.x && first.dmPosition.y == second.dmPosition.y &&
           first.dmDisplayOrientation == second.dmDisplayOrientation && first.dmDisplayFixedOutput == second.dmDisplayFixedOutput &&
           first.dmDisplayFlags == second.dmDisplayFlags;
}

[[nodiscard]] DEVMODEW QueryMode(const std::wstring& name)
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    Require(EnumDisplaySettingsExW(name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE, "EnumDisplaySettingsEx current failed");
    return mode;
}

[[nodiscard]] std::vector<DisplaySettings> SaveDisplays()
{
    std::vector<DisplaySettings> result;
    for (DWORD index = 0; index < 64; index++)
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
        {
            Require(!result.empty(), "BLOCKED: no attached display");
            return result;
        }
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 && (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0)
        {
            Require(result.size() < 16, "BLOCKED: too many attached displays for the bounded mode gate");
            result.push_back({device.DeviceName, QueryMode(device.DeviceName), (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0});
        }
    }
    throw std::runtime_error("BLOCKED: display device enumeration exceeded its bound");
}

[[nodiscard]] std::string DescribeMode(const DisplaySettings& display, const DEVMODEW& mode)
{
    return Utf8(display.name) + " width=" + std::to_string(mode.dmPelsWidth) + " height=" + std::to_string(mode.dmPelsHeight) +
           " hz=" + std::to_string(mode.dmDisplayFrequency) + " bpp=" + std::to_string(mode.dmBitsPerPel) + " x=" + std::to_string(mode.dmPosition.x) +
           " y=" + std::to_string(mode.dmPosition.y) + " orientation=" + std::to_string(mode.dmDisplayOrientation) +
           " fixedOutput=" + std::to_string(mode.dmDisplayFixedOutput) + " flags=" + std::to_string(mode.dmDisplayFlags) +
           " fields=" + std::to_string(mode.dmFields) + " primary=" + std::to_string(display.primary);
}

class ModeRestorer
{
public:
    ModeRestorer(const std::vector<DisplaySettings>& original, Evidence& evidence) : original_(original), evidence_(evidence)
    {
    }
    ~ModeRestorer()
    {
        if (armed_)
        {
            try
            {
                if (!Restore())
                {
                    std::cerr << "FATAL: original display settings could not be verified after emergency restoration\n";
                }
            }
            catch (...)
            {
                std::cerr << "FATAL: emergency restoration/evidence failed\n";
            }
        }
    }
    void Arm() noexcept
    {
        armed_ = true;
    }
    [[nodiscard]] bool Restore()
    {
        // Exact saved current DEVMODE, not the registry default. No registry,
        // primary-display, unsafe-mode, or persistent topology flags are used.
        bool restored = false;
        for (unsigned int attempt = 0; attempt < 2 && !restored; attempt++)
        {
            bool applied = true;
            for (const auto& display : original_)
            {
                DEVMODEW actual{};
                actual.dmSize = sizeof(actual);
                if (!EnumDisplaySettingsExW(display.name.c_str(), ENUM_CURRENT_SETTINGS, &actual, 0) || !SameMode(actual, display.mode))
                {
                    DEVMODEW saved = display.mode;
                    applied = ChangeDisplaySettingsExW(display.name.c_str(), &saved, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL && applied;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const auto current = SaveDisplays();
            restored = applied && current.size() == original_.size();
            for (const auto& display : original_)
            {
                const auto found = std::find_if(current.begin(), current.end(),
                                                [&display](const auto& candidate)
                                                {
                                                    return candidate.name == display.name;
                                                });
                restored = found != current.end() && found->primary == display.primary && SameMode(found->mode, display.mode) && restored;
            }
        }
        armed_ = !restored;
        evidence_.Note(restored ? "RESTORE_VERIFIED: all saved outputs, refresh, position, orientation, bpp, flags and primary status unchanged"
                                : "RESTORE_FAILED: saved display settings do not match current settings");
        return restored;
    }

private:
    const std::vector<DisplaySettings>& original_;
    Evidence& evidence_;
    bool armed_ = false;
};

[[nodiscard]] std::wstring QuoteArgument(const std::wstring_view argument)
{
    Require(argument.size() <= 8192, "command argument too long");
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            backslashes++;
            continue;
        }
        result.append(backslashes * (character == L'\"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'\"')
        {
            result.push_back(L'\\');
        }
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

class ChildProcess
{
public:
    explicit ChildProcess(std::wstring const& command)
    {
        Require(command.size() < 32767, "child command line too long");
        // CreateProcessW mutates the command line in place (module-name
        // quoting), so hand it an explicit mutable copy; the caller's string
        // is never modified.
        std::wstring line = command;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // Suppress only a console window. STARTF_USESHOWWINDOW/SW_HIDE would
        // also override the child's first ShowWindow and invalidate the gate.
        Require(CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information_) != FALSE,
                "CreateProcess mode child failed");
        CloseHandle(information_.hThread);
        information_.hThread = nullptr;
    }
    ~ChildProcess()
    {
        if (WaitForSingleObject(information_.hProcess, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(information_.hProcess, 74);
            WaitForSingleObject(information_.hProcess, 5000);
        }
        CloseHandle(information_.hProcess);
    }
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return information_.hProcess;
    }
    [[nodiscard]] DWORD WaitForExit(const DWORD timeout)
    {
        Require(WaitForSingleObject(information_.hProcess, timeout) == WAIT_OBJECT_0, "mode child did not exit before deadline");
        DWORD code = STILL_ACTIVE;
        Require(GetExitCodeProcess(information_.hProcess, &code) != FALSE, "mode child exit query failed");
        return code;
    }
    void KillTimedOut()
    {
        Require(WaitForSingleObject(information_.hProcess, 1000) == WAIT_TIMEOUT, "timeout fixture exited before its deadline");
        Require(TerminateProcess(information_.hProcess, 74) != FALSE, "timed-out child termination failed");
        Require(WaitForExit(5000) == 74, "timed-out child exit status mismatch");
    }

private:
    PROCESS_INFORMATION information_{};
};

void WaitForSignal(const HANDLE event, const ChildProcess& child, const char* description)
{
    const std::array<HANDLE, 2> handles{event, child.Get()};
    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 15000);
    Require(result == WAIT_OBJECT_0, std::string("mode child signal failed/child exited/timeout: ") + description + " wait=" + std::to_string(result));
}

[[nodiscard]] std::wstring NewEventPrefix()
{
    const auto random = pbprotocol::GenerateRandomSessionId();
    Require(static_cast<bool>(random), "OS CSPRNG failed for test coordination names");
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name = L"Local\\PixelBridge.PresentationGate.";
    for (const auto byte : random.Value().bytes)
    {
        const auto value = std::to_integer<unsigned int>(byte);
        name.push_back(digits[value >> 4]);
        name.push_back(digits[value & 15]);
    }
    return name;
}

[[nodiscard]] HANDLE CreateSignal(const std::wstring& name)
{
    const HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    const DWORD error = GetLastError();
    if (handle == nullptr || error == ERROR_ALREADY_EXISTS)
    {
        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
        throw std::runtime_error("could not create fresh test coordination event");
    }
    return handle;
}

}

void RunModeSupervisor(const std::filesystem::path& root, Evidence& evidence)
{
    const auto original = SaveDisplays();
    for (const auto& display : original)
    {
        evidence.Note("ORIGINAL " + DescribeMode(display, display.mode));
    }
    const auto target = std::find_if(original.begin(), original.end(),
                                     [](const auto& display)
                                     {
                                         return display.mode.dmPelsWidth == 2560 && display.mode.dmPelsHeight == 1440 && display.mode.dmDisplayFrequency == 180;
                                     });
    Require(target != original.end(), "BLOCKED: no current 2560x1440@180 output for the approved 180->120->original gate");
    bool supported = false;
    for (DWORD index = 0; index < 4096; index++)
    {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);
        if (!EnumDisplaySettingsExW(target->name.c_str(), index, &candidate, 0))
        {
            break;
        }
        if (candidate.dmPelsWidth == 2560 && candidate.dmPelsHeight == 1440 && candidate.dmDisplayFrequency == 120 &&
            candidate.dmBitsPerPel == target->mode.dmBitsPerPel && candidate.dmDisplayOrientation == target->mode.dmDisplayOrientation)
        {
            supported = true;
            break;
        }
    }
    Require(supported, "BLOCKED: no enumerated safe 2560x1440@120 alternative");
    DEVMODEW alternative = target->mode;
    alternative.dmDisplayFrequency = 120;
    alternative.dmFields |= DM_DISPLAYFREQUENCY;
    Require(ChangeDisplaySettingsExW(target->name.c_str(), &alternative, nullptr, CDS_TEST, nullptr) == DISP_CHANGE_SUCCESSFUL,
            "BLOCKED: CDS_TEST rejected the candidate mode; nothing was changed");
    evidence.Note("CDS_TEST PASS " + DescribeMode(*target, alternative));
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(length > 0 && length < executable.size(), "cannot determine mode child executable");
    ModeRestorer restorer(original, evidence);
    for (const std::wstring behavior : {L"normal", L"failure", L"crash", L"timeout"})
    {
        const std::wstring prefix = NewEventPrefix();
        const Handle ready(CreateSignal(prefix + L".ready"));
        const Handle changed(CreateSignal(prefix + L".changed"));
        const Handle restored(CreateSignal(prefix + L".restored"));
        const std::wstring command = QuoteArgument(executable.data()) + L" --mode-child " + QuoteArgument(root.wstring()) + L" " + QuoteArgument(prefix) +
                                     L" " + QuoteArgument(target->name) + L" 180 120 " + behavior;
        ChildProcess child(command);
        WaitForSignal(ready.Get(), child, "ready before mode switch");
        evidence.Note("CHILD_READY behavior=" + Utf8(behavior));
        restorer.Arm();
        Require(ChangeDisplaySettingsExW(target->name.c_str(), &alternative, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL,
                "dynamic mode change failed; restore guard is armed");
        const auto changedMode = QueryMode(target->name);
        Require(SameMode(changedMode, alternative), "driver did not enter the requested mode");
        evidence.Note("CHANGED_VERIFIED " + DescribeMode(*target, changedMode));
        WaitForSignal(changed.Get(), child, "new mode and presentation epoch observed");
        if (behavior == L"normal")
        {
            Require(restorer.Restore(), "normal-path display restoration failed");
            WaitForSignal(restored.Get(), child, "restored mode and second fresh epoch");
            Require(child.WaitForExit(15000) == 0, "normal mode child failed");
        }
        else
        {
            if (behavior == L"timeout")
            {
                child.KillTimedOut();
            }
            else
            {
                const DWORD expected = behavior == L"failure" ? 19u : 73u;
                Require(child.WaitForExit(15000) == expected, "failure/crash fixture did not reach its exact injected exit");
            }
            Require(restorer.Restore(), "failure-path display restoration failed");
        }
        evidence.Note("supervised " + Utf8(behavior) + " path PASS");
    }
    Require(restorer.Restore(), "final display restoration verification failed");
}

int RunModeChild(const int argumentCount, wchar_t* arguments[])
{
    using namespace pbrenderd3d;
    Require(argumentCount == 8, "invalid private mode-child invocation");
    const std::filesystem::path root(arguments[2]);
    const std::wstring prefix(arguments[3]);
    const std::wstring displayName(arguments[4]);
    Require(prefix.starts_with(L"Local\\PixelBridge.PresentationGate.") && prefix.size() < 128 && displayName.size() < 32,
            "invalid private coordination identifiers");
    Require(std::wstring_view(arguments[5]) == L"180" && std::wstring_view(arguments[6]) == L"120", "invalid approved mode transition");
    const std::wstring_view behavior(arguments[7]);
    Require(behavior == L"normal" || behavior == L"failure" || behavior == L"crash" || behavior == L"timeout", "invalid mode fault fixture");
    Evidence evidence(root, "mode-child-" + Utf8(behavior));
    const Handle ready(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (prefix + L".ready").c_str()));
    const Handle changed(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (prefix + L".changed").c_str()));
    const Handle restored(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (prefix + L".restored").c_str()));
    Require(ready.Get() != nullptr && changed.Get() != nullptr && restored.Get() != nullptr, "supervisor must exist before the mode child");
    const auto monitors = GetMonitors();
    const auto target = std::find_if(monitors.begin(), monitors.end(),
                                     [&displayName](const auto& monitor)
                                     {
                                         return displayName == monitor.info.szDevice;
                                     });
    Require(target != monitors.end(), "target output not found by child");
    DataWindowConfig config;
    config.clientOrigin = GetOrigin(*target, config.width, config.height);
    const auto window = CreateDataWindow(config);
    const std::size_t pitch = static_cast<std::size_t>(config.width) * 4;
    auto pixels = MakePixelOracle(config.width, config.height, pitch, 15);
    std::uint64_t sequence = 0;
    for (unsigned int frame = 0; frame < 6; frame++)
    {
        StampSequence(pixels, sequence);
        PresentAndVerify(*window, config, pixels, pitch, sequence++, evidence);
    }
    const auto before = window->GetSnapshot();
    evidence.Record("before-parent-mode-change", before);
    Require(before.environment.modeFrequency == 180 && SetEvent(ready.Get()) != FALSE, "child initial mode/signal mismatch");
    const auto newMode = WaitFor(
        *window,
        [&before](const auto& snapshot)
        {
            return snapshot.environment.modeFrequency == 120 && snapshot.timing.presentationEpoch > before.timing.presentationEpoch &&
                   snapshot.candidateContractSatisfied;
        },
        "actual 120Hz and new epoch", std::chrono::seconds(15));
    Require(!newMode.timing.presentedVisualFps && !newMode.timing.presentQueueLatencyMs && newMode.timing.observedPresents == 0,
            "mode change kept stale timing measurements");
    evidence.Record("120Hz-new-epoch-old-samples-invalidated", newMode);
    for (unsigned int frame = 0; frame < 8; frame++)
    {
        StampSequence(pixels, sequence);
        PresentAndVerify(*window, config, pixels, pitch, sequence++, evidence);
    }
    const auto beforeRestore = window->GetSnapshot();
    evidence.Record("120Hz-warmup-attempt", beforeRestore);
    Require(SetEvent(changed.Get()) != FALSE, "changed-mode signal failed");
    if (behavior == L"failure")
    {
        evidence.Note("injected child failure exit=19; parent must restore");
        return 19;
    }
    if (behavior == L"crash")
    {
        evidence.Note("injected abrupt child termination exit=73; no child destructors; parent must restore");
        TerminateProcess(GetCurrentProcess(), 73);
        return 73;
    }
    if (behavior == L"timeout")
    {
        evidence.Note("injected child stall; parent's deadline must terminate and restore");
        Sleep(INFINITE);
        return 74;
    }
    const auto originalMode = WaitFor(
        *window,
        [&beforeRestore](const auto& snapshot)
        {
            return snapshot.environment.modeFrequency == 180 && snapshot.timing.presentationEpoch > beforeRestore.timing.presentationEpoch &&
                   snapshot.candidateContractSatisfied;
        },
        "restored 180Hz and fresh epoch", std::chrono::seconds(15));
    Require(!originalMode.timing.presentedVisualFps && !originalMode.timing.presentQueueLatencyMs && originalMode.timing.observedPresents == 0,
            "restoration retained 120Hz timing observations");
    evidence.Record("180Hz-restored-old-samples-invalidated", originalMode);
    for (unsigned int frame = 0; frame < 8; frame++)
    {
        StampSequence(pixels, sequence);
        PresentAndVerify(*window, config, pixels, pitch, sequence++, evidence);
    }
    const auto final = window->GetSnapshot();
    evidence.Record("restored-mode-warmup-attempt", final);
    evidence.Note(std::string("restored DXGI timing state=") + pbpresenttiming::GetTimingStateName(final.timing.state) +
                  " issue=" + pbpresenttiming::GetTimingIssueName(final.timing.issue));
    window->Stop();
    Require(SetEvent(restored.Get()) != FALSE, "restored signal failed");
    evidence.Note("PASS mode/epoch/re-warmup; measurement availability separately reported");
    return 0;
}

}
