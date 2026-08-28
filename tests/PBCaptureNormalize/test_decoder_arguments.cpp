#include "../../apps/PixelBridgeDecoder/capture_bootstrap_arguments.h"
#include "../../apps/PixelBridgeDecoder/capture_bootstrap_telemetry.h"
#include "../../apps/common/diagnostic_file.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using pbdecoder::BootstrapBackend;
using pbdecoder::CaptureBootstrapArguments;
using pbdecoder::ParseCaptureBootstrapArguments;

bool SameArguments(const CaptureBootstrapArguments& first, const CaptureBootstrapArguments& second)
{
    return first.backend == second.backend && first.seconds == second.seconds && first.physicalRoi == second.physicalRoi &&
           first.hasRoi == second.hasRoi && first.showHelp == second.showHelp && first.telemetryPath == second.telemetryPath;
}

bool Parse(const std::vector<std::wstring>& arguments, CaptureBootstrapArguments& output)
{
    std::vector<const wchar_t*> pointers;
    pointers.reserve(arguments.size());
    for (const auto& argument : arguments)
    {
        pointers.push_back(argument.c_str());
    }
    return ParseCaptureBootstrapArguments(static_cast<int>(pointers.size()), pointers.data(), output);
}

class TemporaryLog
{
public:
    TemporaryLog()
    {
        std::array<wchar_t, MAX_PATH> directory{};
        const auto count = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
        REQUIRE(count > 0);
        REQUIRE(count < directory.size());
        REQUIRE(GetTempFileNameW(directory.data(), L"pbd", 0, name_.data()) != 0);
        reservation = name_.data();
        logPath = reservation.native() + L".jsonl";
    }
    ~TemporaryLog()
    {
        DeleteFileW(logPath.c_str());
        DeleteFileW(reservation.c_str());
    }
    std::array<wchar_t, MAX_PATH> name_{};
    std::filesystem::path reservation;
    std::filesystem::path logPath;
};
} // namespace

TEST_CASE("Decoder Bootstrap requires an explicit backend and has a finite diagnostic duration", "[bootstrap-cli]")
{
    CaptureBootstrapArguments output;
    const std::vector<std::wstring> wgc{L"decoder", L"--capture-bootstrap", L"--backend", L"wgc"};
    REQUIRE(Parse(wgc, output));
    CHECK(output.backend == BootstrapBackend::Wgc);
    CHECK(output.seconds == 10);
    CHECK_FALSE(output.hasRoi);
    CHECK_FALSE(output.showHelp);
    CHECK(output.telemetryPath == nullptr);

    const std::vector<std::wstring> dxgi{L"decoder", L"--capture-bootstrap", L"--telemetry", L"observations.jsonl", L"--roi", L"-2000", L"-100", L"-80", L"980",
                                       L"--seconds", L"600", L"--backend", L"dxgi"};
    REQUIRE(Parse(dxgi, output));
    CHECK(output.backend == BootstrapBackend::Dxgi);
    CHECK(output.seconds == 600);
    CHECK(output.hasRoi);
    CHECK(output.physicalRoi == std::array<std::int32_t, 4>{-2000, -100, -80, 980});
    CHECK(output.telemetryPath == dxgi[3].c_str());
    CHECK(std::wstring_view(output.telemetryPath) == L"observations.jsonl");

    for (const wchar_t* help : {L"--help", L"-h"})
    {
        REQUIRE(Parse({L"decoder", L"--capture-bootstrap", help}, output));
        CHECK(output.showHelp);
        CHECK_FALSE(output.hasRoi);
        CHECK(output.telemetryPath == nullptr);
    }
    wchar_t name[] = L"decoder";
    wchar_t command[] = L"--capture-bootstrap";
    wchar_t backendOption[] = L"--backend";
    wchar_t backend[] = L"wgc";
    wchar_t* entry[]{name, command, backendOption, backend};
    REQUIRE(ParseCaptureBootstrapArguments(4, entry, output));
    CHECK_FALSE(output.showHelp);
}

TEST_CASE("Decoder Bootstrap rejects duplicate, missing, unknown and payload identity arguments atomically", "[bootstrap-cli]")
{
    const CaptureBootstrapArguments sentinel{BootstrapBackend::Dxgi, 27, {-7, -8, 9, 10}, true, true, L"untouched"};
    const std::vector<std::vector<std::wstring>> invalid{
        {L"decoder"}, {L"decoder", L"--capture-bootstrap"}, {L"decoder", L"--backend", L"dxgi"},
        {L"decoder", L"--capture-bootstrap", L"--backend"}, {L"decoder", L"--capture-bootstrap", L"--backend", L"WGC"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"dxgi", L"--backend", L"wgc"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--capture-bootstrap"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--seconds"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--seconds", L"1", L"--seconds", L"2"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--roi", L"0", L"0", L"1"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--roi", L"0", L"0", L"1", L"1", L"--roi", L"0", L"0", L"2", L"2"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--telemetry"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--telemetry", L""},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--telemetry", L"--seconds"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--telemetry", L"a", L"--telemetry", L"b"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--help"},
        {L"decoder", L"--capture-bootstrap", L"--help", L"extra"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--session-tag", L"1"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--frame-sequence", L"1"},
        {L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--profile", L"1"}};
    for (const auto& arguments : invalid)
    {
        auto output = sentinel;
        REQUIRE_FALSE(Parse(arguments, output));
        CHECK(SameArguments(output, sentinel));
    }
    auto output = sentinel;
    REQUIRE_FALSE(ParseCaptureBootstrapArguments(4, nullptr, output));
    const wchar_t* nullArgument[]{L"decoder", L"--capture-bootstrap", L"--backend", nullptr};
    REQUIRE_FALSE(ParseCaptureBootstrapArguments(4, nullArgument, output));
    CHECK(SameArguments(output, sentinel));
}

TEST_CASE("Decoder physical coordinates and duration reject overflow without signed wrap", "[bootstrap-cli]")
{
    CaptureBootstrapArguments output;
    for (const auto seconds : {L"1", L"600"})
    {
        REQUIRE(Parse({L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--seconds", seconds}, output));
    }
    for (const auto seconds : {L"", L"0", L"-1", L"+1", L"601", L"4294967296", L"18446744073709551616", L"1x", L" 1", L"1 "})
    {
        const auto before = output;
        REQUIRE_FALSE(Parse({L"decoder", L"--capture-bootstrap", L"--backend", L"wgc", L"--seconds", seconds}, output));
        CHECK(SameArguments(output, before));
    }
    REQUIRE(Parse({L"decoder", L"--capture-bootstrap", L"--backend", L"dxgi", L"--roi", L"-2147483648", L"-2147483648", L"-2147467264", L"-2147467264"}, output));
    CHECK(output.physicalRoi[0] == std::numeric_limits<std::int32_t>::min());
    REQUIRE(Parse({L"decoder", L"--capture-bootstrap", L"--backend", L"dxgi", L"--roi", L"2147483646", L"2147483646", L"2147483647", L"2147483647"}, output));
    CHECK(output.physicalRoi[2] == std::numeric_limits<std::int32_t>::max());
    for (const auto& coordinates : std::vector<std::array<std::wstring, 4>>{
        {L"-2147483649", L"0", L"1", L"1"}, {L"0", L"0", L"2147483648", L"1"}, {L"-2147483648", L"0", L"2147483647", L"1"},
        {L"0", L"0", L"16385", L"1"}, {L"0", L"0", L"1", L"16385"}, {L"0", L"0", L"0", L"1"}, {L"0", L"0", L"1", L"0"},
        {L"10", L"0", L"1", L"1"}, {L"--", L"0", L"1", L"1"}, {L"+0", L"0", L"1", L"1"}, {L"0", L"0", L"1.5", L"1"}})
    {
        const auto before = output;
        REQUIRE_FALSE(Parse({L"decoder", L"--capture-bootstrap", L"--backend", L"dxgi", L"--roi", coordinates[0], coordinates[1], coordinates[2], coordinates[3]}, output));
        CHECK(SameArguments(output, before));
    }
}

TEST_CASE("Diagnostic logs are create-new, bounded and explicitly finalized without truncating prior evidence", "[bootstrap-cli][io]")
{
    TemporaryLog paths;
    {
        std::ofstream original(paths.reservation, std::ios::binary);
        original << "do not overwrite";
    }
    REQUIRE_THROWS_AS(pbdiagnostic::DiagnosticFile(paths.reservation.c_str()), std::runtime_error);
    {
        std::ifstream original(paths.reservation, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(original)), {});
        CHECK(text == "do not overwrite");
    }
    {
        pbdiagnostic::DiagnosticFile file(paths.logPath.c_str());
        file.Write("{\"first\":true}\n");
        REQUIRE_THROWS_AS(pbdiagnostic::DiagnosticFile(paths.logPath.c_str()), std::runtime_error);
        const std::string oversized(pbdiagnostic::DiagnosticFile::maximumBytes, 'x');
        REQUIRE_THROWS_AS(file.Write(oversized), std::runtime_error);
        file.Write("{\"second\":true}\n");
        file.Finish();
        file.Finish();
    }
    std::ifstream saved(paths.logPath, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(saved)), {});
    CHECK(contents == "{\"first\":true}\n{\"second\":true}\n");
    pbdiagnostic::DiagnosticFile disabled(nullptr);
    disabled.Write("no file");
    disabled.Finish();
}

TEST_CASE("Bootstrap telemetry preserves original domains and represents invalid floating diagnostics as null", "[bootstrap-cli][telemetry]")
{
    pbdecoder::BootstrapDiagnosticEvent event;
    event.capture.domain.sourceId[0] = std::byte{0xfe};
    event.capture.domain.sourceId[15] = std::byte{0xab};
    event.capture.domain.captureEpoch = std::numeric_limits<std::uint64_t>::max();
    event.capture.captureObservation = std::numeric_limits<std::uint64_t>::max();
    event.capture.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
    event.capture.physicalRoi = {-1920, -1080, 0, 0};
    event.visual.geometry.originX = std::numeric_limits<double>::quiet_NaN();
    event.visual.geometry.originY = std::numeric_limits<double>::infinity();
    event.visual.geometry.scaleX = 0.5;
    event.visual.geometry.scaleY = 2.0;
    event.disposition = pbdecoder::BootstrapDisposition::VisualErasure;
    const auto text = pbdecoder::SerializeBootstrapDiagnosticEvent(event);
    CHECK(text.find("\"sourceId\":\"fe0000000000000000000000000000ab\"") != std::string::npos);
    CHECK(text.find("\"captureEpoch\":\"18446744073709551615\"") != std::string::npos);
    CHECK(text.find("\"identity\":null") != std::string::npos);
    CHECK(text.find("\"originX\":null,\"originY\":null,\"scaleX\":0.5,\"scaleY\":2") != std::string::npos);
    CHECK(text.find("\"physicalRoi\":[-1920,-1080,0,0]") != std::string::npos);
    CHECK(text.back() == '\n');
    CHECK(text.find('\n') == text.size() - 1);
    REQUIRE_THROWS_AS(pbdecoder::SerializeCaptureBootstrapSnapshot("bad\"\n", {}, {}, {}, {}, 0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(pbdecoder::SerializeCaptureBootstrapSnapshot(nullptr, {}, {}, {}, {}, 0, 0), std::invalid_argument);
}

TEST_CASE("Decoder lifecycle does not abort a native runtime's bounded device recovery on a transient error snapshot", "[bootstrap-cli][recovery]")
{
    using namespace pbcapturenormalize;
    CaptureSnapshot capture;
    DiagnosticReadbackSnapshot readback;
    for (const auto state : {CaptureState::Starting, CaptureState::Running, CaptureState::Draining, CaptureState::Recreating, CaptureState::WaitingForEnvironment})
    {
        capture.state = state;
        capture.error = CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion, static_cast<std::int32_t>(DXGI_ERROR_DEVICE_REMOVED));
        CHECK_FALSE(pbdecoder::IsTerminalDiagnosticFailure(capture, readback));
        capture.error = {};
        CHECK_FALSE(pbdecoder::IsTerminalDiagnosticFailure(capture, readback));
    }
    for (const auto state : {CaptureState::Failed, CaptureState::Stopped})
    {
        capture.state = state;
        CHECK(pbdecoder::IsTerminalDiagnosticFailure(capture, readback));
    }
    capture.state = CaptureState::Running;
    readback.error = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    CHECK(pbdecoder::IsTerminalDiagnosticFailure(capture, readback));
}

TEST_CASE("Decoder final stdout flush failure cannot be reported as diagnostic success", "[bootstrap-cli][io]")
{
    class FailingFlushBuffer final : public std::stringbuf
    {
    public:
        int sync() override
        {
            return -1;
        }
    };
    FailingFlushBuffer buffer;
    std::ostream output(&buffer);
    output << "{\"kind\":\"capture-final\"}\n";
    REQUIRE(output.good());
    REQUIRE_THROWS_AS(pbdiagnostic::FlushDiagnosticOutput(output), std::runtime_error);
    CHECK(output.bad());
    CHECK(buffer.str() == "{\"kind\":\"capture-final\"}\n");
    std::ostringstream healthy;
    healthy << "{\"accepted\":\"1\"}\n";
    REQUIRE_NOTHROW(pbdiagnostic::FlushDiagnosticOutput(healthy));
    CHECK(healthy.good());
    CHECK(healthy.str() == "{\"accepted\":\"1\"}\n");
}
