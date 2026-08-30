#include "phase1_file_gate_arguments.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

namespace
{

[[nodiscard]] bool Parse(const std::vector<std::wstring>& values, phase1gate::Arguments& output)
{
    std::vector<wchar_t*> arguments;
    arguments.reserve(values.size());
    for (const std::wstring& value : values)
    {
        arguments.push_back(const_cast<wchar_t*>(value.c_str()));
    }
    return phase1gate::ParseArguments(static_cast<int>(arguments.size()), arguments.data(), output);
}

}

TEST_CASE("Phase1 file sender arguments are explicit and bounded", "[phase1][file-gate][arguments]")
{
    phase1gate::Arguments parsed;
    REQUIRE(Parse({L"gate", L"--sender", L"--profile", L"desktop-levels-2x2", L"--source-new",
        L"source.bin", L"--origin", L"2560", L"0", L"--telemetry-new", L"sender.jsonl", L"--maximum-sender-seconds", L"300"}, parsed));
    CHECK(parsed.mode == phase1gate::ProcessMode::Sender);
    CHECK(parsed.profile == phase1gate::PhysicalProfile::DesktopLevels2);
    CHECK(parsed.sourcePath == L"source.bin");
    CHECK(parsed.clientOrigin == std::array<std::int32_t, 2>{2560, 0});
    CHECK(parsed.maximumSenderSeconds == 300);
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--origin", L"2560", L"0", L"--telemetry-new", L"sender.jsonl", L"--backend", L"wgc"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--source-new", L"other.bin", L"--origin", L"2560", L"0", L"--telemetry-new", L"sender.jsonl"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--origin", L"2560", L"0", L"--telemetry-new", L"sender.jsonl", L"--maximum-sender-seconds", L"3601"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--telemetry-new", L"sender.jsonl"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--origin", L"2560", L"0", L"--origin", L"0", L"0", L"--telemetry-new", L"sender.jsonl"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--sender", L"--profile", L"shape-chroma", L"--source-new",
        L"source.bin", L"--origin", L"2147483648", L"0", L"--telemetry-new", L"sender.jsonl"}, parsed));
}

TEST_CASE("Phase1 file receiver requires an exact signed physical 1080p ROI", "[phase1][file-gate][arguments][roi]")
{
    phase1gate::Arguments parsed;
    REQUIRE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"dxgi",
        L"--roi", L"-1920", L"-100", L"0", L"980", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl", L"--timeout-seconds", L"600",
        L"--restart-after-unique", L"30", L"--soak-seconds", L"300"}, parsed));
    CHECK(parsed.mode == phase1gate::ProcessMode::Receiver);
    CHECK(parsed.profile == phase1gate::PhysicalProfile::ShapeChroma);
    CHECK(parsed.backend == phase1gate::CaptureBackend::Dxgi);
    CHECK(parsed.physicalRoi == std::array<std::int32_t, 4>{-1920, -100, 0, 980});
    CHECK(parsed.timeoutSeconds == 600);
    CHECK(parsed.restartAfterUniqueFrames == 30);
    CHECK(parsed.soakSeconds == 300);
    CHECK_FALSE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"wgc",
        L"--roi", L"0", L"0", L"1919", L"1080", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"wgc",
        L"--roi", L"0", L"0", L"1920", L"1080", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl", L"--timeout-seconds", L"300", L"--soak-seconds", L"300"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"wgc",
        L"--roi", L"0", L"0", L"1920", L"1080", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl", L"--restart-after-unique", L"0"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"wgc",
        L"--roi", L"0", L"0", L"1920", L"1080", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl", L"--soak-seconds", L"299"}, parsed));
    CHECK_FALSE(Parse({L"gate", L"--receiver", L"--profile", L"shape-chroma", L"--backend", L"wgc",
        L"--roi", L"0", L"0", L"1920", L"1080", L"--origin", L"2560", L"0", L"--output-new", L"output.bin",
        L"--telemetry-new", L"receiver.jsonl"}, parsed));
}

TEST_CASE("Phase1 numeric parsing rejects overflow and preserves output", "[phase1][file-gate][arguments][bounds]")
{
    std::uint64_t unsignedValue = 17;
    CHECK_FALSE(phase1gate::ParseUnsigned(L"18446744073709551616", UINT64_MAX, unsignedValue));
    CHECK(unsignedValue == 17);
    CHECK_FALSE(phase1gate::ParseUnsigned(L"9", 5, unsignedValue));
    CHECK(unsignedValue == 17);
    std::int32_t signedValue = 19;
    REQUIRE(phase1gate::ParseInt32(L"-2147483648", signedValue));
    CHECK(signedValue == INT32_MIN);
    signedValue = 19;
    CHECK_FALSE(phase1gate::ParseInt32(L"2147483648", signedValue));
    CHECK(signedValue == 19);

    phase1gate::Arguments arguments;
    CHECK_FALSE(phase1gate::ParseArguments(0, nullptr, arguments));
    std::array<wchar_t*, 2> missingArgument{const_cast<wchar_t*>(L"gate"), nullptr};
    CHECK_FALSE(phase1gate::ParseArguments(static_cast<int>(missingArgument.size()), missingArgument.data(), arguments));
}
