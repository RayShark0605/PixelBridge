#pragma once

#include <cstdint>
#include <string_view>

namespace pbencoder
{

enum class PresentationVisual
{
    ReferenceRaster,
    LocalDesktopBootstrap,
    DesktopLevels2,
    DesktopLevels4
};

struct DataWindowArguments
{
    bool showHelp = false;
    bool dataWindow = false;
    bool hasFrameCount = false;
    std::uint64_t frameLimit = 0;
    bool hasSequenceInterval = false;
    // DesktopLevels only: dwell after each accepted submission. 500 ms is the
    // reference carousel rate; the native gate selects a slower rate whose
    // dwell exceeds the measured diagnostic readback period.
    std::uint32_t sequenceIntervalMilliseconds = 500;
    const wchar_t* telemetryPath = nullptr;
    PresentationVisual visual = PresentationVisual::ReferenceRaster;
    bool operator==(const DataWindowArguments&) const = default;
};

[[nodiscard]] inline bool ParseFrameCount(const std::wstring_view text, std::uint64_t& result) noexcept
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
        if (value > (1000000 - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0)
    {
        return false;
    }
    result = value;
    return true;
}

// Bounded 50..60000 ms; output unchanged on failure.
[[nodiscard]] inline bool ParseSequenceIntervalMilliseconds(const std::wstring_view text, std::uint32_t& result) noexcept
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
        if (value > (60000 - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value < 50)
    {
        return false;
    }
    result = static_cast<std::uint32_t>(value);
    return true;
}

// Output is unchanged on failure. telemetryPath borrows the argv storage;
// no renderer, file, random session or other resource is created by parsing.
[[nodiscard]] inline bool ParseDataWindowArguments(const int argumentCount, const wchar_t* const arguments[], DataWindowArguments& output) noexcept
{
    if (argumentCount < 1 || arguments == nullptr)
    {
        return false;
    }
    for (int index = 0; index < argumentCount; index++)
    {
        if (arguments[index] == nullptr)
        {
            return false;
        }
    }
    DataWindowArguments parsed;
    if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--help")
    {
        parsed.showHelp = true;
        output = parsed;
        return true;
    }
    bool explicitDataWindow = false;
    bool explicitVisual = false;
    for (int index = 1; index < argumentCount; index++)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--data-window" && !explicitDataWindow)
        {
            explicitDataWindow = true;
            parsed.dataWindow = true;
        }
        else if (argument == L"--visual" && !explicitVisual && index + 1 < argumentCount)
        {
            index++;
            const std::wstring_view visual(arguments[index]);
            if (visual != L"local-desktop-bootstrap" && visual != L"desktop-levels-2x2" && visual != L"desktop-levels-4x4")
            {
                return false;
            }
            explicitVisual = true;
            parsed.visual = visual == L"local-desktop-bootstrap" ? PresentationVisual::LocalDesktopBootstrap :
                visual == L"desktop-levels-2x2" ? PresentationVisual::DesktopLevels2 : PresentationVisual::DesktopLevels4;
            parsed.dataWindow = true;
        }
        else if (argument == L"--frames" && !parsed.hasFrameCount && index + 1 < argumentCount)
        {
            index++;
            if (!ParseFrameCount(arguments[index], parsed.frameLimit))
            {
                return false;
            }
            parsed.hasFrameCount = true;
        }
        else if (argument == L"--sequence-interval-ms" && !parsed.hasSequenceInterval && index + 1 < argumentCount)
        {
            index++;
            if (!ParseSequenceIntervalMilliseconds(arguments[index], parsed.sequenceIntervalMilliseconds))
            {
                return false;
            }
            parsed.hasSequenceInterval = true;
        }
        else if (argument == L"--telemetry" && parsed.telemetryPath == nullptr && index + 1 < argumentCount)
        {
            index++;
            parsed.telemetryPath = arguments[index];
            if (*parsed.telemetryPath == 0)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    if (!parsed.dataWindow)
    {
        return false;
    }
    output = parsed;
    return true;
}

} // namespace pbencoder
