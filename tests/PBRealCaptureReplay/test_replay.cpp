#include "pbrealcapturereplay/replay_file.h"

#include "pbdemodd3d11/demodulator.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;

std::uint16_t LoadUint16(const std::span<const std::byte> bytes, const std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t LoadUint32(const std::span<const std::byte> bytes, const std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; index++)
    {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

std::uint64_t LoadUint64(const std::span<const std::byte> bytes, const std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; index++)
    {
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

void StoreUint32(const std::span<std::byte> bytes, const std::size_t offset, const std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
}

std::uint32_t OracleCrc32c(const std::span<const std::byte> bytes)
{
    std::uint32_t crc = 0xFFFFFFFF;
    for (const auto value : bytes)
    {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (std::size_t bit = 0; bit < 8; bit++)
        {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

void FlipFileByte(const std::filesystem::path& path, const std::uint64_t offset)
{
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    REQUIRE(stream.read(&value, 1));
    value ^= 0x40;
    stream.seekp(static_cast<std::streamoff>(offset));
    REQUIRE(stream.write(&value, 1));
    stream.flush();
    REQUIRE(stream.good());
}

void RecomputeIndependentChecksums(const std::filesystem::path& path, const bool recordChecksum)
{
    const auto fileBytes = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(fileBytes));
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    }
    const std::size_t footerOffset = bytes.size() - pbrealcapturereplay::kReplayFileFooterBytes;
    const std::uint64_t bytesBeforeFooter = LoadUint64(bytes, footerOffset + 16);
    REQUIRE(bytesBeforeFooter == footerOffset);
    const std::size_t frameHeaderOffset = pbrealcapturereplay::kReplayFileHeaderBytes;
    StoreUint32(bytes, frameHeaderOffset + 508,
        OracleCrc32c(std::span<const std::byte>(bytes).subspan(frameHeaderOffset, 508)));
    if (recordChecksum)
    {
        const std::size_t recordStart = pbrealcapturereplay::kReplayFileHeaderBytes;
        const std::size_t recordTrailer = footerOffset - 4;
        StoreUint32(bytes, recordTrailer, OracleCrc32c(std::span<const std::byte>(bytes).subspan(recordStart, recordTrailer - recordStart)));
    }
    StoreUint32(bytes, footerOffset + 24, OracleCrc32c(std::span<const std::byte>(bytes).first(footerOffset)));
    StoreUint32(bytes, footerOffset + 36, OracleCrc32c(std::span<const std::byte>(bytes).subspan(footerOffset, 36)));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    output.flush();
    REQUIRE(output.good());
}

class TemporaryReplayFiles
{
public:
    TemporaryReplayFiles()
    {
        static std::atomic<std::uint32_t> counter{0};
        root_ = std::filesystem::temp_directory_path() /
            (L"pixelbridge-real-capture-replay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter.fetch_add(1)));
    }

    ~TemporaryReplayFiles()
    {
        for (const auto& path : paths_)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            auto partial = path;
            partial += L".partial";
            error.clear();
            std::filesystem::remove(partial, error);
        }
    }

    std::filesystem::path Make(const wchar_t* const suffix)
    {
        auto path = root_;
        path += suffix;
        paths_.push_back(path);
        return path;
    }

private:
    std::filesystem::path root_;
    std::vector<std::filesystem::path> paths_;
};

struct D3DEnvironment
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
};

D3DEnvironment CreateWarpEnvironment()
{
    D3DEnvironment environment;
    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &environment.device, &selectedLevel, &environment.context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data() + 1, 1, D3D11_SDK_VERSION, &environment.device, &selectedLevel, &environment.context);
    }
    REQUIRE(SUCCEEDED(result));
    REQUIRE(selectedLevel >= D3D_FEATURE_LEVEL_11_0);
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    REQUIRE(SUCCEEDED(environment.device.As(&dxgiDevice)));
    REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter->GetDesc(&description)));
    environment.adapterLuid = description.AdapterLuid;
    return environment;
}

std::array<std::byte, 44> MakeBootstrap(const std::uint64_t profileId, const std::uint8_t layoutVersion,
    const std::uint64_t sequence, const std::uint64_t sessionTag)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = layoutVersion;
    record.visualProfileId = profileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = sequence;
    std::array<std::byte, 44> output{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, output));
    return output;
}

struct FrameFixture
{
    std::uint64_t frameSequence = 0;
    std::array<std::byte, 44> bootstrap{};
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    std::uint32_t dpiX = 144;
    std::uint32_t dpiY = 144;
    double scaleX = 1.5;
    double scaleY = 1.5;
    std::string displayIdentity = "DISPLAY\\WARP-REFERENCE";
    pbrealcapturereplay::ReplayPresentationMetadata presentation;
    std::vector<std::byte> sender;
    std::vector<std::byte> captured;

    pbrealcapturereplay::ReplayFrameView View() const
    {
        return {frameSequence, bootstrap, capture, dpiX, dpiY, scaleX, scaleY, displayIdentity, presentation,
            {1920, 1080, 1920 * 4, DXGI_FORMAT_B8G8R8A8_UNORM, sender},
            {1920, 1080, 1920 * 4, DXGI_FORMAT_B8G8R8A8_UNORM, captured}};
    }
};

FrameFixture MakeFixture(const std::uint64_t profileId, const std::uint8_t layoutVersion,
    const std::uint32_t dataBytes, const bool shapeChroma, const std::uint64_t sequence,
    const std::uint64_t sessionTag, const std::uint64_t observation, const LUID adapterLuid)
{
    FrameFixture fixture;
    fixture.frameSequence = sequence;
    fixture.bootstrap = MakeBootstrap(profileId, layoutVersion, sequence, sessionTag);
    std::vector<std::byte> logicalData(dataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(fixture.bootstrap, logicalData));
    fixture.sender.resize(pbmodulation::kLocalDesktopFrameBgraBytes);
    if (shapeChroma)
    {
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(fixture.bootstrap, logicalData, fixture.sender));
    }
    else
    {
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(fixture.bootstrap, logicalData, fixture.sender));
    }
    fixture.captured = fixture.sender;
    auto& capture = fixture.capture;
    capture.domain.sourceId[0] = std::byte{0xA5};
    capture.domain.sourceId[15] = std::byte{0x5A};
    capture.domain.captureEpoch = 11;
    capture.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
    capture.captureObservation = observation;
    capture.sourceGeneration = 3;
    capture.slotGeneration = observation + 17;
    capture.slotIndex = static_cast<std::uint32_t>(observation % 3);
    capture.physicalRoi = {-1920, 0, 0, 1080};
    capture.sourceContentSize = {1920, 1080};
    capture.sourceExtent = {1920, 1080};
    capture.roiSize = {1920, 1080};
    capture.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
    capture.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
    capture.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    capture.adapterLuid = adapterLuid;
    capture.bitsPerColor = 8;
    capture.outputColorSpace = 0;
    capture.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    capture.hdr = false;
    capture.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
    capture.timestamp.rawValue = 10000000 + static_cast<std::int64_t>(observation) * 100000;
    capture.timestamp.rawFrequency = 10000000;
    capture.timestamp.monotonic100ns = capture.timestamp.rawValue;
    capture.timestamp.arrivalQpc100ns = capture.timestamp.monotonic100ns + 100;
    capture.roiCopyTime100ns = 321 + observation;
    capture.isCursorExcluded = true;
    capture.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    capture.pointer.positionKnown = true;
    capture.pointer.physicalLeft = -100;
    capture.pointer.physicalTop = 200;
    capture.pointer.rawUpdateTimestamp = capture.timestamp.rawValue - 1;
    fixture.presentation.available = true;
    fixture.presentation.presentationEpoch = 4;
    fixture.presentation.qpcFrequency = 10000000;
    fixture.presentation.sample.frameSequence = sequence;
    fixture.presentation.sample.beginQpc = 9000000 + static_cast<std::int64_t>(observation) * 100000;
    fixture.presentation.sample.endQpc = fixture.presentation.sample.beginQpc + 1000;
    fixture.presentation.sample.outcome = pbpresenttiming::PresentOutcome::Success;
    fixture.presentation.sample.presentId = static_cast<std::uint32_t>(observation + 100);
    return fixture;
}

struct OracleResult
{
    pbdesktoplevels::FrameEvaluation evaluation;
    std::vector<pbdesktoplevels::AcceptedTransportBlock> accepted;
};

OracleResult RunCpuOracle(const std::uint64_t profileId, const std::span<const std::byte> pixels)
{
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const pbmodulation::LumaView view{pixels, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
    OracleResult result;
    if (profileId == pbmodulation::kShapeChromaProfileId)
    {
        const auto observation = channel.DecodeShapeChroma(view);
        REQUIRE(observation.modulation.IsAccepted());
        result.evaluation = observation.evaluation;
    }
    else
    {
        const auto observation = channel.Decode(view);
        REQUIRE(observation.modulation.IsAccepted());
        result.evaluation = observation.evaluation;
    }
    REQUIRE(result.evaluation.IsVerified());
    const auto accepted = channel.GetAcceptedTransportBlocks();
    result.accepted.assign(accepted.begin(), accepted.end());
    return result;
}

ComPtr<ID3D11Texture2D> UploadTexture(ID3D11Device* const device, const std::span<const std::byte> pixels)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1920;
    description.Height = 1080;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), 1920 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)));
    return texture;
}

pbdemodd3d11::DemodFrameResult RunGpuReplay(D3DEnvironment& environment,
    pbdemodd3d11::Demodulator& demodulator, const pbrealcapturereplay::ReplayFrame& replay)
{
    const auto texture = UploadTexture(environment.device.Get(), replay.capturedRoi.pixels);
    pbcapturenormalize::ScreenCaptureFrame frame{replay.capture, texture.Get()};
    pbdemodd3d11::DemodSubmission submission;
    REQUIRE(demodulator.Submit(frame, environment.context.Get(), replay.canonicalBootstrap, submission));
    environment.context->Flush();
    pbdemodd3d11::DemodFrameResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const auto poll = demodulator.Poll(environment.context.Get(), submission, result);
        if (poll.ready)
        {
            REQUIRE(poll.status);
            return result;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

pbrealcapturereplay::ReplayFileDescriptor MakeDescriptor(const std::uint32_t frames)
{
    pbrealcapturereplay::ReplayFileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::LocalDesktopExactCandidate;
    descriptor.datasetId[0] = std::byte{0x31};
    descriptor.datasetId[15] = std::byte{0xA7};
    descriptor.expectedFrameCount = frames;
    return descriptor;
}

void WriteDataset(const std::filesystem::path& path, const pbrealcapturereplay::ReplayFileDescriptor& descriptor,
    const std::span<const FrameFixture> fixtures)
{
    std::unique_ptr<pbrealcapturereplay::ReplayWriter> writer;
    REQUIRE(pbrealcapturereplay::ReplayWriter::Create(path, descriptor, {}, writer));
    REQUIRE(writer != nullptr);
    for (const auto& fixture : fixtures)
    {
        REQUIRE(writer->Append(fixture.View()));
    }
    REQUIRE(writer->Finalize());
    const auto snapshot = writer->GetSnapshot();
    REQUIRE(snapshot.complete);
    REQUIRE(snapshot.framesProcessed == fixtures.size());
    REQUIRE(snapshot.fileBytes == std::filesystem::file_size(path));
}

} // namespace

TEST_CASE("Real Capture Replay preserves versioned evidence and replays CPU/GPU demod plus exact accepted Transport blocks without presentation",
    "[replay][roundtrip][demod][d3d11][warp][fec]")
{
    TemporaryReplayFiles temporary;
    const auto path = temporary.Make(L"-roundtrip.pbrcr");
    auto environment = CreateWarpEnvironment();
    const auto* directProfile = pbmodulation::GetDesktopLevelsProfile(pbmodulation::kDesktopLevels4ProfileId);
    REQUIRE(directProfile != nullptr);
    std::array fixtures{
        MakeFixture(pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
            pbmodulation::kShapeChromaDataBytes, true, 7, 0x1020304050607080ULL, 1, environment.adapterLuid),
        MakeFixture(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
            directProfile->dataBytes, false, 8, 0x1122334455667788ULL, 2, environment.adapterLuid)};
    const auto descriptor = MakeDescriptor(static_cast<std::uint32_t>(fixtures.size()));
    WriteDataset(path, descriptor, fixtures);

    std::array<std::byte, pbrealcapturereplay::kReplayFileHeaderBytes> header{};
    std::ifstream headerStream(path, std::ios::binary);
    REQUIRE(headerStream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())));
    REQUIRE(std::string(reinterpret_cast<const char*>(header.data()), 8) == "PBRCR001");
    REQUIRE(LoadUint16(header, 8) == pbrealcapturereplay::kReplayFormatVersion);
    REQUIRE(LoadUint16(header, 10) == pbrealcapturereplay::kReplayFileHeaderBytes);
    REQUIRE(LoadUint32(header, 12) == 0x01020304);
    REQUIRE(LoadUint32(header, 16) == static_cast<std::uint32_t>(descriptor.datasetClass));
    REQUIRE(LoadUint32(header, 20) == fixtures.size());
    REQUIRE(std::equal(descriptor.datasetId.begin(), descriptor.datasetId.end(), header.begin() + 24));
    REQUIRE(LoadUint64(header, 40) == pbrealcapturereplay::kReplayFileHeaderBytes);
    REQUIRE(LoadUint32(header, 92) == OracleCrc32c(std::span<const std::byte>(header).first(92)));

    std::unique_ptr<pbrealcapturereplay::ReplayReader> reader;
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(path, {}, reader));
    REQUIRE(reader->GetSnapshot().descriptor == descriptor);
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    for (std::size_t index = 0; index < fixtures.size(); index++)
    {
        pbrealcapturereplay::ReplayFrame replay;
        REQUIRE(reader->ReadNext(replay));
        const auto& fixture = fixtures[index];
        REQUIRE(replay.frameSequence == fixture.frameSequence);
        REQUIRE(replay.canonicalBootstrap == fixture.bootstrap);
        REQUIRE(replay.senderCanonicalRaster.pixels == fixture.sender);
        REQUIRE(replay.capturedRoi.pixels == fixture.captured);
        REQUIRE(replay.displayIdentityUtf8 == fixture.displayIdentity);
        REQUIRE(replay.capture.domain == fixture.capture.domain);
        REQUIRE(replay.capture.captureObservation == fixture.capture.captureObservation);
        REQUIRE(replay.capture.timestamp.monotonic100ns == fixture.capture.timestamp.monotonic100ns);
        REQUIRE(replay.capture.roiCopyTime100ns == fixture.capture.roiCopyTime100ns);
        REQUIRE(replay.capture.pointer == fixture.capture.pointer);
        REQUIRE(replay.presentation.available);
        REQUIRE(replay.presentation.sample.frameSequence == fixture.frameSequence);
        REQUIRE(replay.presentation.sample.presentId == fixture.presentation.sample.presentId);
        const auto parsed = pbprotocol::ParseBootstrapRecord(replay.canonicalBootstrap);
        REQUIRE(parsed);
        const auto oracle = RunCpuOracle(parsed.Value().visualProfileId, replay.capturedRoi.pixels);
        const auto gpu = RunGpuReplay(environment, *demodulator, replay);
        REQUIRE(gpu.evaluation.IsVerified());
        REQUIRE(gpu.acceptedTransportBlockCount == oracle.accepted.size());
        for (std::size_t block = 0; block < oracle.accepted.size(); block++)
        {
            REQUIRE(gpu.acceptedTransportBlocks[block] == oracle.accepted[block]);
        }
    }
    pbrealcapturereplay::ReplayFrame unchanged;
    unchanged.frameSequence = 0xDEADBEEF;
    REQUIRE(reader->ReadNext(unchanged).code == pbrealcapturereplay::ReplayError::EndOfFile);
    REQUIRE(unchanged.frameSequence == 0xDEADBEEF);
    REQUIRE(reader->GetSnapshot().complete);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("Real Capture Replay fails closed on overwrite, incomplete publication, corruption, truncation and allocation limits",
    "[replay][file-safety][checksum][bounds]")
{
    TemporaryReplayFiles temporary;
    const auto path = temporary.Make(L"-safety.pbrcr");
    const auto incompletePath = temporary.Make(L"-incomplete.pbrcr");
    const auto corruptPath = temporary.Make(L"-corrupt.pbrcr");
    const auto recordCorruptPath = temporary.Make(L"-record-corrupt.pbrcr");
    const auto digestCorruptPath = temporary.Make(L"-digest-corrupt.pbrcr");
    const auto reservedCorruptPath = temporary.Make(L"-reserved-corrupt.pbrcr");
    const auto outcomeCorruptPath = temporary.Make(L"-outcome-corrupt.pbrcr");
    const auto truncatedPath = temporary.Make(L"-truncated.pbrcr");
    auto environment = CreateWarpEnvironment();
    std::array fixtures{MakeFixture(pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
        pbmodulation::kShapeChromaDataBytes, true, 19, 0x8877665544332211ULL, 1, environment.adapterLuid)};
    const auto descriptor = MakeDescriptor(1);

    std::unique_ptr<pbrealcapturereplay::ReplayWriter> writer;
    REQUIRE(pbrealcapturereplay::ReplayWriter::Create(path, descriptor, {}, writer));
    auto invalid = fixtures[0].View();
    invalid.frameSequence++;
    REQUIRE(writer->Append(invalid).code == pbrealcapturereplay::ReplayError::InvalidArgument);
    REQUIRE(writer->Append(fixtures[0].View()));
    REQUIRE(writer->Finalize());
    std::unique_ptr<pbrealcapturereplay::ReplayWriter> unchangedWriter;
    REQUIRE(pbrealcapturereplay::ReplayWriter::Create(path, descriptor, {}, unchangedWriter).code ==
        pbrealcapturereplay::ReplayError::AlreadyExists);
    REQUIRE(unchangedWriter == nullptr);

    {
        std::unique_ptr<pbrealcapturereplay::ReplayWriter> incomplete;
        REQUIRE(pbrealcapturereplay::ReplayWriter::Create(incompletePath, descriptor, {}, incomplete));
        auto partialPath = incompletePath;
        partialPath += L".partial";
        REQUIRE(std::filesystem::exists(partialPath));
    }
    auto partialPath = incompletePath;
    partialPath += L".partial";
    REQUIRE_FALSE(std::filesystem::exists(incompletePath));
    REQUIRE_FALSE(std::filesystem::exists(partialPath));

    REQUIRE(std::filesystem::copy_file(path, corruptPath));
    const std::uint64_t mutationOffset = pbrealcapturereplay::kReplayFileHeaderBytes +
        pbrealcapturereplay::kReplayFrameHeaderBytes + fixtures[0].displayIdentity.size() + 4096;
    FlipFileByte(corruptPath, mutationOffset);
    std::unique_ptr<pbrealcapturereplay::ReplayReader> reader;
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(corruptPath, {}, reader).code ==
        pbrealcapturereplay::ReplayError::ChecksumMismatch);
    REQUIRE(reader == nullptr);

    REQUIRE(std::filesystem::copy_file(path, recordCorruptPath));
    FlipFileByte(recordCorruptPath, mutationOffset);
    RecomputeIndependentChecksums(recordCorruptPath, false);
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(recordCorruptPath, {}, reader));
    pbrealcapturereplay::ReplayFrame recordUnchanged;
    recordUnchanged.frameSequence = 0xABCDEF;
    REQUIRE(reader->ReadNext(recordUnchanged).code == pbrealcapturereplay::ReplayError::ChecksumMismatch);
    REQUIRE(recordUnchanged.frameSequence == 0xABCDEF);
    reader.reset();

    REQUIRE(std::filesystem::copy_file(path, digestCorruptPath));
    FlipFileByte(digestCorruptPath, mutationOffset);
    RecomputeIndependentChecksums(digestCorruptPath, true);
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(digestCorruptPath, {}, reader));
    pbrealcapturereplay::ReplayFrame digestUnchanged;
    digestUnchanged.frameSequence = 0x13579BDF;
    REQUIRE(reader->ReadNext(digestUnchanged).code == pbrealcapturereplay::ReplayError::DigestMismatch);
    REQUIRE(digestUnchanged.frameSequence == 0x13579BDF);
    reader.reset();

    const std::uint64_t frameHeaderOffset = pbrealcapturereplay::kReplayFileHeaderBytes;
    REQUIRE(std::filesystem::copy_file(path, reservedCorruptPath));
    FlipFileByte(reservedCorruptPath, frameHeaderOffset + 441);
    RecomputeIndependentChecksums(reservedCorruptPath, true);
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(reservedCorruptPath, {}, reader));
    pbrealcapturereplay::ReplayFrame reservedUnchanged;
    reservedUnchanged.frameSequence = 0x2468ACE0;
    REQUIRE(reader->ReadNext(reservedUnchanged).code == pbrealcapturereplay::ReplayError::MalformedFrame);
    REQUIRE(reservedUnchanged.frameSequence == 0x2468ACE0);
    reader.reset();

    REQUIRE(std::filesystem::copy_file(path, outcomeCorruptPath));
    FlipFileByte(outcomeCorruptPath, frameHeaderOffset + 373);
    RecomputeIndependentChecksums(outcomeCorruptPath, true);
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(outcomeCorruptPath, {}, reader));
    pbrealcapturereplay::ReplayFrame outcomeUnchanged;
    outcomeUnchanged.frameSequence = 0x10203040;
    REQUIRE(reader->ReadNext(outcomeUnchanged).code == pbrealcapturereplay::ReplayError::MalformedFrame);
    REQUIRE(outcomeUnchanged.frameSequence == 0x10203040);
    reader.reset();

    REQUIRE(std::filesystem::copy_file(path, truncatedPath));
    std::filesystem::resize_file(truncatedPath, std::filesystem::file_size(truncatedPath) - 1);
    const auto truncated = pbrealcapturereplay::ReplayReader::Open(truncatedPath, {}, reader);
    REQUIRE_FALSE(truncated);
    REQUIRE(reader == nullptr);

    auto fileLimit = pbrealcapturereplay::ReplayLimits{};
    fileLimit.maximumFileBytes = std::filesystem::file_size(path) - 1;
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(path, fileLimit, reader).code ==
        pbrealcapturereplay::ReplayError::ResourceLimit);
    REQUIRE(reader == nullptr);

    auto frameLimit = pbrealcapturereplay::ReplayLimits{};
    frameLimit.maximumRasterBytesPerFrame = 1;
    REQUIRE(pbrealcapturereplay::ReplayReader::Open(path, frameLimit, reader));
    pbrealcapturereplay::ReplayFrame unchanged;
    unchanged.frameSequence = 0x12345678;
    REQUIRE(reader->ReadNext(unchanged).code == pbrealcapturereplay::ReplayError::ResourceLimit);
    REQUIRE(unchanged.frameSequence == 0x12345678);
    REQUIRE(unchanged.senderCanonicalRaster.pixels.empty());
}
