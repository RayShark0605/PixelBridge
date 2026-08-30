#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace phase1gate
{

enum class ProcessMode : std::uint8_t
{
    Sender,
    Receiver
};

enum class PhysicalProfile : std::uint8_t
{
    DesktopLevels2,
    ShapeChroma
};

enum class CaptureBackend : std::uint8_t
{
    Wgc,
    Dxgi
};

struct Arguments
{
    ProcessMode mode = ProcessMode::Sender;
    PhysicalProfile profile = PhysicalProfile::DesktopLevels2;
    CaptureBackend backend = CaptureBackend::Wgc;
    std::wstring sourcePath;
    std::wstring outputPath;
    std::wstring telemetryPath;
    std::array<std::int32_t, 4> physicalRoi{};
    std::array<std::int32_t, 2> clientOrigin{};
    std::uint32_t timeoutSeconds = 90;
    std::uint32_t maximumSenderSeconds = 120;
    std::uint32_t restartAfterUniqueFrames = 30;
    std::uint32_t soakSeconds = 0;
    bool hasRoi = false;
    bool hasClientOrigin = false;
    bool showHelp = false;
};

[[nodiscard]] inline bool ParseUnsigned(const std::wstring_view text, const std::uint64_t maximum,
    std::uint64_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (digit > maximum || value > (maximum - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    output = value;
    return true;
}

[[nodiscard]] inline bool ParseInt32(const std::wstring_view text, std::int32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const bool negative = text.front() == L'-';
    const std::wstring_view magnitude = negative ? text.substr(1) : text;
    const std::uint64_t maximum = negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1ULL :
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    std::uint64_t parsed = 0;
    if (!ParseUnsigned(magnitude, maximum, parsed))
    {
        return false;
    }
    if (negative && parsed == maximum)
    {
        output = std::numeric_limits<std::int32_t>::min();
    }
    else
    {
        const std::int32_t narrowed = static_cast<std::int32_t>(parsed);
        output = negative ? -narrowed : narrowed;
    }
    return true;
}

[[nodiscard]] inline bool ParseArguments(const int argumentCount, wchar_t* arguments[], Arguments& output)
{
    if (argumentCount <= 0 || arguments == nullptr)
    {
        return false;
    }
    for (int index = 1; index < argumentCount; index++)
    {
        if (arguments[index] == nullptr)
        {
            return false;
        }
    }
    if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--help")
    {
        Arguments parsed;
        parsed.showHelp = true;
        output = std::move(parsed);
        return true;
    }
    Arguments parsed;
    bool hasMode = false;
    bool hasProfile = false;
    bool hasBackend = false;
    bool hasSource = false;
    bool hasOutput = false;
    bool hasTelemetry = false;
    bool hasTimeout = false;
    bool hasMaximumSender = false;
    bool hasRestart = false;
    bool hasSoak = false;
    for (int index = 1; index < argumentCount; index++)
    {
        const std::wstring_view argument(arguments[index]);
        if ((argument == L"--sender" || argument == L"--receiver") && !hasMode)
        {
            parsed.mode = argument == L"--sender" ? ProcessMode::Sender : ProcessMode::Receiver;
            hasMode = true;
        }
        else if (argument == L"--profile" && !hasProfile && index + 1 < argumentCount)
        {
            const std::wstring_view profile(arguments[++index]);
            if (profile != L"desktop-levels-2x2" && profile != L"shape-chroma")
            {
                return false;
            }
            parsed.profile = profile == L"desktop-levels-2x2" ? PhysicalProfile::DesktopLevels2 : PhysicalProfile::ShapeChroma;
            hasProfile = true;
        }
        else if (argument == L"--backend" && !hasBackend && index + 1 < argumentCount)
        {
            const std::wstring_view backend(arguments[++index]);
            if (backend != L"wgc" && backend != L"dxgi")
            {
                return false;
            }
            parsed.backend = backend == L"wgc" ? CaptureBackend::Wgc : CaptureBackend::Dxgi;
            hasBackend = true;
        }
        else if (argument == L"--source-new" && !hasSource && index + 1 < argumentCount)
        {
            parsed.sourcePath = arguments[++index];
            hasSource = !parsed.sourcePath.empty();
        }
        else if (argument == L"--output-new" && !hasOutput && index + 1 < argumentCount)
        {
            parsed.outputPath = arguments[++index];
            hasOutput = !parsed.outputPath.empty();
        }
        else if (argument == L"--telemetry-new" && !hasTelemetry && index + 1 < argumentCount)
        {
            parsed.telemetryPath = arguments[++index];
            hasTelemetry = !parsed.telemetryPath.empty();
        }
        else if (argument == L"--roi" && !parsed.hasRoi && index + 4 < argumentCount)
        {
            for (std::size_t coordinate = 0; coordinate < parsed.physicalRoi.size(); coordinate++)
            {
                if (!ParseInt32(arguments[++index], parsed.physicalRoi[coordinate]))
                {
                    return false;
                }
            }
            parsed.hasRoi = true;
        }
        else if (argument == L"--origin" && !parsed.hasClientOrigin && index + 2 < argumentCount)
        {
            for (std::size_t coordinate = 0; coordinate < parsed.clientOrigin.size(); coordinate++)
            {
                index++;
                if (!ParseInt32(arguments[index], parsed.clientOrigin[coordinate]))
                {
                    return false;
                }
            }
            parsed.hasClientOrigin = true;
        }
        else if (argument == L"--timeout-seconds" && !hasTimeout && index + 1 < argumentCount)
        {
            std::uint64_t value = 0;
            if (!ParseUnsigned(arguments[++index], 1800, value) || value == 0)
            {
                return false;
            }
            parsed.timeoutSeconds = static_cast<std::uint32_t>(value);
            hasTimeout = true;
        }
        else if (argument == L"--maximum-sender-seconds" && !hasMaximumSender && index + 1 < argumentCount)
        {
            std::uint64_t value = 0;
            if (!ParseUnsigned(arguments[++index], 3600, value) || value == 0)
            {
                return false;
            }
            parsed.maximumSenderSeconds = static_cast<std::uint32_t>(value);
            hasMaximumSender = true;
        }
        else if (argument == L"--restart-after-unique" && !hasRestart && index + 1 < argumentCount)
        {
            std::uint64_t value = 0;
            if (!ParseUnsigned(arguments[++index], 1000000, value) || value == 0)
            {
                return false;
            }
            parsed.restartAfterUniqueFrames = static_cast<std::uint32_t>(value);
            hasRestart = true;
        }
        else if (argument == L"--soak-seconds" && !hasSoak && index + 1 < argumentCount)
        {
            std::uint64_t value = 0;
            if (!ParseUnsigned(arguments[++index], 1800, value) || (value != 0 && value < 300))
            {
                return false;
            }
            parsed.soakSeconds = static_cast<std::uint32_t>(value);
            hasSoak = true;
        }
        else
        {
            return false;
        }
    }
    if (!hasMode || !hasProfile || !hasTelemetry)
    {
        return false;
    }
    if (parsed.mode == ProcessMode::Sender)
    {
        if (!hasSource || !parsed.hasClientOrigin || hasBackend || hasOutput || parsed.hasRoi || hasTimeout || hasRestart || hasSoak)
        {
            return false;
        }
    }
    else if (!hasBackend || !hasOutput || !parsed.hasRoi || hasSource || parsed.hasClientOrigin || hasMaximumSender)
    {
        return false;
    }
    const std::int64_t roiWidth = static_cast<std::int64_t>(parsed.physicalRoi[2]) - parsed.physicalRoi[0];
    const std::int64_t roiHeight = static_cast<std::int64_t>(parsed.physicalRoi[3]) - parsed.physicalRoi[1];
    if (parsed.mode == ProcessMode::Receiver && (roiWidth != 1920 || roiHeight != 1080 ||
        parsed.soakSeconds >= parsed.timeoutSeconds))
    {
        return false;
    }
    output = std::move(parsed);
    return true;
}

} // namespace phase1gate
