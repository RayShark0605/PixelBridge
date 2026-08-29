#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace pbdecoder
{

enum class BootstrapBackend : std::uint8_t
{
    Wgc, Dxgi
};

struct CaptureBootstrapArguments
{
    BootstrapBackend backend = BootstrapBackend::Wgc;
    std::uint32_t seconds = 10;
    std::array<std::int32_t, 4> physicalRoi{};
    bool hasRoi = false;
    bool showHelp = false;
    const wchar_t* telemetryPath = nullptr;
    bool desktopLevels = false;
    bool shapeChroma = false;
};

namespace detail
{
inline bool ParseBoundedDecimal(const std::wstring_view text, const std::uint64_t maximum, std::uint64_t& output) noexcept
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
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
        {
            return false;
        }
        value = value * 10 + digit;
    }
    output = value;
    return true;
}

inline bool ParsePhysicalCoordinate(std::wstring_view text, std::int32_t& output) noexcept
{
    const bool negative = !text.empty() && text.front() == L'-';
    if (negative)
    {
        text.remove_prefix(1);
    }
    const std::uint64_t maximum = negative ? 2147483648ull : 2147483647ull;
    std::uint64_t magnitude = 0;
    if (!ParseBoundedDecimal(text, maximum, magnitude))
    {
        return false;
    }
    const auto signedValue = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    output = static_cast<std::int32_t>(signedValue);
    return true;
}
} // namespace detail

// argv remains owned by wmain for the entire run. Failed parsing changes no
// caller state and performs no display, allocation, file or capture operation.
inline bool ParseCaptureBootstrapArguments(const int argumentCount, const wchar_t* const arguments[], CaptureBootstrapArguments& output) noexcept
{
    if (argumentCount < 2 || arguments == nullptr || arguments[1] == nullptr ||
        (std::wstring_view(arguments[1]) != L"--capture-bootstrap" && std::wstring_view(arguments[1]) != L"--capture-desktop-levels" &&
         std::wstring_view(arguments[1]) != L"--capture-shape-chroma"))
    {
        return false;
    }
    CaptureBootstrapArguments parsed;
    parsed.desktopLevels = std::wstring_view(arguments[1]) == L"--capture-desktop-levels";
    parsed.shapeChroma = std::wstring_view(arguments[1]) == L"--capture-shape-chroma";
    if (argumentCount == 3 && arguments[2] != nullptr && (std::wstring_view(arguments[2]) == L"--help" || std::wstring_view(arguments[2]) == L"-h"))
    {
        parsed.showHelp = true;
        output = parsed;
        return true;
    }
    bool hasBackend = false;
    bool hasSeconds = false;
    bool hasTelemetry = false;
    for (int index = 2; index < argumentCount; index++)
    {
        if (arguments[index] == nullptr)
        {
            return false;
        }
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--backend" && !hasBackend)
        {
            if (index == argumentCount - 1 || arguments[index + 1] == nullptr)
            {
                return false;
            }
            index++;
            const std::wstring_view backend(arguments[index]);
            if (backend != L"wgc" && backend != L"dxgi")
            {
                return false;
            }
            parsed.backend = backend == L"wgc" ? BootstrapBackend::Wgc : BootstrapBackend::Dxgi;
            hasBackend = true;
        }
        else if (argument == L"--seconds" && !hasSeconds)
        {
            if (index == argumentCount - 1 || arguments[index + 1] == nullptr)
            {
                return false;
            }
            index++;
            std::uint64_t seconds = 0;
            if (!detail::ParseBoundedDecimal(arguments[index], 600, seconds) || seconds == 0)
            {
                return false;
            }
            parsed.seconds = static_cast<std::uint32_t>(seconds);
            hasSeconds = true;
        }
        else if (argument == L"--roi" && !parsed.hasRoi)
        {
            if (argumentCount - index <= 4)
            {
                return false;
            }
            for (std::size_t coordinate = 0; coordinate < parsed.physicalRoi.size(); coordinate++)
            {
                index++;
                if (arguments[index] == nullptr || !detail::ParsePhysicalCoordinate(arguments[index], parsed.physicalRoi[coordinate]))
                {
                    return false;
                }
            }
            const auto width = static_cast<std::int64_t>(parsed.physicalRoi[2]) - parsed.physicalRoi[0];
            const auto height = static_cast<std::int64_t>(parsed.physicalRoi[3]) - parsed.physicalRoi[1];
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            {
                return false;
            }
            parsed.hasRoi = true;
        }
        else if (argument == L"--telemetry" && !hasTelemetry)
        {
            if (index == argumentCount - 1 || arguments[index + 1] == nullptr || arguments[index + 1][0] == L'\0')
            {
                return false;
            }
            index++;
            if (std::wstring_view(arguments[index]).starts_with(L"--"))
            {
                return false;
            }
            parsed.telemetryPath = arguments[index];
            hasTelemetry = true;
        }
        else
        {
            return false;
        }
    }
    if (!hasBackend)
    {
        return false;
    }
    output = parsed;
    return true;
}

} // namespace pbdecoder
