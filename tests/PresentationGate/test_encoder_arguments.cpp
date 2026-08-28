#include "../../apps/PixelBridgeEncoder/presentation_cli_arguments.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using pbencoder::DataWindowArguments;
using pbencoder::PresentationVisual;

bool Parse(const std::initializer_list<const wchar_t*> options, DataWindowArguments& output)
{
    std::vector<const wchar_t*> arguments{L"PixelBridgeEncoder"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return pbencoder::ParseDataWindowArguments(static_cast<int>(arguments.size()), arguments.data(), output);
}

TEST_CASE("Encoder DesktopLevels candidates require explicit names and do not change legacy defaults", "[encoder-cli][desktop-levels]")
{
    for (const auto name : {L"desktop-levels-2x2", L"desktop-levels-4x4"})
    {
        DataWindowArguments parsed;
        REQUIRE(Parse({L"--visual", name, L"--frames", L"64"}, parsed));
        REQUIRE(parsed.dataWindow);
        REQUIRE(parsed.visual == (std::wstring_view(name) == L"desktop-levels-2x2" ? PresentationVisual::DesktopLevels2 : PresentationVisual::DesktopLevels4));
        REQUIRE(parsed.frameLimit == 64);
        const auto saved = parsed;
        REQUIRE_FALSE(Parse({L"--visual", name, L"--visual", L"local-desktop-bootstrap"}, parsed));
        REQUIRE(parsed == saved);
        REQUIRE_FALSE(Parse({L"--visual", L"desktop-levels-8x8"}, parsed));
        REQUIRE(parsed == saved);
        REQUIRE(Parse({L"--data-window"}, parsed));
        REQUIRE(parsed.visual == PresentationVisual::ReferenceRaster);
    }
}

DataWindowArguments Sentinel()
{
    return {true, true, true, 987654, L"unchanged-telemetry.jsonl", PresentationVisual::LocalDesktopBootstrap};
}
}

TEST_CASE("Encoder data-window retains the reference default and existing finite submission options", "[encoder-cli]")
{
    auto output = Sentinel();
    REQUIRE(Parse({L"--data-window"}, output));
    CHECK_FALSE(output.showHelp);
    CHECK(output.dataWindow);
    CHECK_FALSE(output.hasFrameCount);
    CHECK(output.frameLimit == 0);
    CHECK(output.telemetryPath == nullptr);
    CHECK(output.visual == PresentationVisual::ReferenceRaster);

    REQUIRE(Parse({L"--frames", L"12", L"--telemetry", L"呈现-telemetry.jsonl", L"--data-window"}, output));
    CHECK(output.dataWindow);
    CHECK(output.hasFrameCount);
    CHECK(output.frameLimit == 12);
    REQUIRE(output.telemetryPath != nullptr);
    CHECK(std::wstring_view(output.telemetryPath) == L"呈现-telemetry.jsonl");
    CHECK(output.visual == PresentationVisual::ReferenceRaster);
}

TEST_CASE("Encoder only the explicit LocalDesktop visual opts into the new binding", "[encoder-cli]")
{
    for (const auto arguments : {std::initializer_list<const wchar_t*>{L"--visual", L"local-desktop-bootstrap"},
                                {L"--data-window", L"--visual", L"local-desktop-bootstrap"},
                                {L"--visual", L"local-desktop-bootstrap", L"--data-window"}})
    {
        auto output = Sentinel();
        REQUIRE(Parse(arguments, output));
        CHECK(output.dataWindow);
        CHECK(output.visual == PresentationVisual::LocalDesktopBootstrap);
        CHECK_FALSE(output.showHelp);
        CHECK_FALSE(output.hasFrameCount);
        CHECK(output.frameLimit == 0);
        CHECK(output.telemetryPath == nullptr);

        // There is no sticky process/global profile selection. Parsing the old
        // entry point after the opt-in always restores its reference binding.
        REQUIRE(Parse({L"--data-window"}, output));
        CHECK(output.visual == PresentationVisual::ReferenceRaster);
        CHECK_FALSE(output.hasFrameCount);
        CHECK(output.telemetryPath == nullptr);
    }
    DataWindowArguments output;
    REQUIRE(Parse({L"--telemetry", L"D:\\capture evidence\\显式.jsonl", L"--visual", L"local-desktop-bootstrap", L"--frames", L"1000000"}, output));
    CHECK(output.dataWindow);
    CHECK(output.visual == PresentationVisual::LocalDesktopBootstrap);
    CHECK(output.hasFrameCount);
    CHECK(output.frameLimit == 1000000);
    REQUIRE(output.telemetryPath != nullptr);
    CHECK(std::wstring_view(output.telemetryPath) == L"D:\\capture evidence\\显式.jsonl");
}

TEST_CASE("Encoder duplicate malformed missing and mixed help arguments fail without modifying options", "[encoder-cli]")
{
    for (const auto arguments : {std::initializer_list<const wchar_t*>{},
                                {L"--unknown"}, {L"--frames", L"1"}, {L"--telemetry", L"unused.jsonl"},
                                {L"--visual"}, {L"--visual", L""}, {L"--visual", L"reference-raster"},
                                {L"--visual", L"Local-Desktop-Bootstrap"}, {L"--visual", L"local-desktop-bootstrap-extra"},
                                {L"--visual", L"--data-window"}, {L"--data-window", L"--visual"},
                                {L"--visual=local-desktop-bootstrap"},
                                {L"--visual", L"local-desktop-bootstrap", L"--visual", L"local-desktop-bootstrap"},
                                {L"--visual", L"local-desktop-bootstrap", L"--data-window", L"--data-window"},
                                {L"--data-window", L"--data-window"},
                                {L"--visual", L"local-desktop-bootstrap", L"--frames"},
                                {L"--visual", L"local-desktop-bootstrap", L"--frames", L"0"},
                                {L"--visual", L"local-desktop-bootstrap", L"--frames", L"1", L"--frames", L"2"},
                                {L"--data-window", L"--frames", L"1", L"--frames", L"2"},
                                {L"--visual", L"local-desktop-bootstrap", L"--telemetry"},
                                {L"--visual", L"local-desktop-bootstrap", L"--telemetry", L""},
                                {L"--visual", L"local-desktop-bootstrap", L"--telemetry", L"first.jsonl", L"--telemetry", L"second.jsonl"},
                                {L"--help", L"--visual", L"local-desktop-bootstrap"},
                                {L"--visual", L"local-desktop-bootstrap", L"--help"},
                                {L"--help", L"--data-window"}, {L"--data-window", L"--help"},
                                {L"--data-window", L"extra"},
                                {L"--visual", L"local-desktop-bootstrap", L"extra"}})
    {
        auto output = Sentinel();
        const auto before = output;
        REQUIRE_FALSE(Parse(arguments, output));
        CHECK(output == before);
    }
}

TEST_CASE("Encoder frame limits preserve exact boundaries numeric syntax and failure immutability", "[encoder-cli]")
{
    constexpr std::array<std::pair<std::wstring_view, std::uint64_t>, 4> validCounts{{{L"1", 1}, {L"12", 12}, {L"1000000", 1000000}, {L"000001", 1}}};
    for (const auto& [text, expected] : validCounts)
    {
        std::uint64_t value = 77;
        REQUIRE(pbencoder::ParseFrameCount(text, value));
        CHECK(value == expected);
    }
    for (const auto text : {L"", L"0", L"000000", L"-1", L"+1", L"1x", L" 1", L"1 ", L"1.0", L"1e3", L"１",
                            L"1000001", L"18446744073709551615", L"18446744073709551616"})
    {
        std::uint64_t value = std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbencoder::ParseFrameCount(text, value));
        CHECK(value == std::numeric_limits<std::uint64_t>::max());
        auto output = Sentinel();
        const auto before = output;
        REQUIRE_FALSE(Parse({L"--visual", L"local-desktop-bootstrap", L"--frames", text}, output));
        CHECK(output == before);
    }
}

TEST_CASE("Encoder help alone and native mutable argv use the same allocation-free parser", "[encoder-cli]")
{
    auto output = Sentinel();
    REQUIRE(Parse({L"--help"}, output));
    CHECK(output.showHelp);
    CHECK_FALSE(output.dataWindow);
    CHECK_FALSE(output.hasFrameCount);
    CHECK(output.frameLimit == 0);
    CHECK(output.telemetryPath == nullptr);
    CHECK(output.visual == PresentationVisual::ReferenceRaster);

    wchar_t program[] = L"PixelBridgeEncoder";
    wchar_t option[] = L"--data-window";
    wchar_t* arguments[]{program, option};
    REQUIRE(pbencoder::ParseDataWindowArguments(2, arguments, output));
    CHECK(output.dataWindow);
    CHECK(output.visual == PresentationVisual::ReferenceRaster);

    const auto before = output;
    REQUIRE_FALSE(pbencoder::ParseDataWindowArguments(0, arguments, output));
    CHECK(output == before);
    REQUIRE_FALSE(pbencoder::ParseDataWindowArguments(-1, arguments, output));
    CHECK(output == before);
    REQUIRE_FALSE(pbencoder::ParseDataWindowArguments(2, nullptr, output));
    CHECK(output == before);
    arguments[1] = nullptr;
    REQUIRE_FALSE(pbencoder::ParseDataWindowArguments(2, arguments, output));
    CHECK(output == before);
}
