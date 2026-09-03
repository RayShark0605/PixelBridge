#include "pbdemodd3d11/demodulator.h"

#include "../PBModulation/unified_transform_corpus_cases.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/unified_visual.h"
#include "pbmodulation/visual_temporal.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbremotevisualsimulator/channel_transform.h"
#include "pbstorage/output_file.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;

#ifndef PB_G12_PARITY_REPORT
#error PB_G12_PARITY_REPORT must name the generated G12 parity report
#endif

#ifndef PB_G12_PERFORMANCE_REPORT
#error PB_G12_PERFORMANCE_REPORT must name the generated G12 performance report
#endif

#ifndef PB_G12_SCRATCH_ROOT
#error PB_G12_SCRATCH_ROOT must name the G12 scratch root
#endif

constexpr std::uint32_t kAmdVendorId = 0x1002;
constexpr std::uint32_t kNvidiaVendorId = 0x10DE;
constexpr std::size_t kLaneCount = 3;
constexpr std::size_t kTransportPayloadBytes = pbmodulation::kUnifiedInformationBytes -
    pbprotocol::kTransportMinimumBlockBytes;

static_assert(kTransportPayloadBytes == 1314);

struct D3DEnvironment
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
    D3D_FEATURE_LEVEL featureLevel{};
};

struct AdapterFingerprint
{
    std::string backend;
    std::uint32_t adapterIndex = UINT32_MAX;
    DXGI_ADAPTER_DESC1 description{};
    bool softwareRasterizer = false;
    bool driverVersionAvailable = false;
    std::uint64_t driverVersion = 0;
};

struct HardwareAdapterCandidate
{
    ComPtr<IDXGIAdapter1> adapter;
    AdapterFingerprint fingerprint;
    HRESULT deviceCreateStatus = E_FAIL;
    D3D_FEATURE_LEVEL featureLevel{};
};

[[nodiscard]] D3DEnvironment CreateWarpEnvironment()
{
    D3DEnvironment environment;
    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &environment.device, &environment.featureLevel, &environment.context);
    if (status == E_INVALIDARG)
    {
        status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data() + 1, 1, D3D11_SDK_VERSION,
            &environment.device, &environment.featureLevel, &environment.context);
    }
    REQUIRE(SUCCEEDED(status));
    REQUIRE(environment.featureLevel >= D3D_FEATURE_LEVEL_11_0);
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    REQUIRE(SUCCEEDED(environment.device.As(&dxgiDevice)));
    REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter->GetDesc(&description)));
    environment.adapterLuid = description.AdapterLuid;
    return environment;
}

[[nodiscard]] HRESULT CreateHardwareEnvironment(IDXGIAdapter1* const adapter,
    D3DEnvironment& output) noexcept
{
    if (adapter == nullptr)
    {
        return E_INVALIDARG;
    }
    D3DEnvironment environment;
    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT status = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels.data(), static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION, &environment.device, &environment.featureLevel, &environment.context);
    if (status == E_INVALIDARG)
    {
        status = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels.data() + 1, 1, D3D11_SDK_VERSION,
            &environment.device, &environment.featureLevel, &environment.context);
    }
    if (FAILED(status))
    {
        return status;
    }
    DXGI_ADAPTER_DESC1 description{};
    status = adapter->GetDesc1(&description);
    if (FAILED(status))
    {
        return status;
    }
    environment.adapterLuid = description.AdapterLuid;
    output = std::move(environment);
    return S_OK;
}

[[nodiscard]] AdapterFingerprint GetAdapterFingerprint(IDXGIAdapter1* const adapter,
    const std::string_view backend, const std::uint32_t adapterIndex, const bool softwareRasterizer)
{
    REQUIRE(adapter != nullptr);
    AdapterFingerprint fingerprint;
    fingerprint.backend = backend;
    fingerprint.adapterIndex = adapterIndex;
    fingerprint.softwareRasterizer = softwareRasterizer;
    REQUIRE(SUCCEEDED(adapter->GetDesc1(&fingerprint.description)));
    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
    {
        fingerprint.driverVersionAvailable = true;
        fingerprint.driverVersion = static_cast<std::uint64_t>(version.QuadPart);
    }
    return fingerprint;
}

[[nodiscard]] AdapterFingerprint GetEnvironmentFingerprint(const D3DEnvironment& environment,
    const std::string_view backend, const bool softwareRasterizer)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    REQUIRE(SUCCEEDED(environment.device.As(&dxgiDevice)));
    REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter.As(&adapter1)));
    return GetAdapterFingerprint(adapter1.Get(), backend, UINT32_MAX, softwareRasterizer);
}

[[nodiscard]] std::vector<HardwareAdapterCandidate> EnumerateHardwareAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    std::vector<HardwareAdapterCandidate> candidates;
    candidates.reserve(16);
    for (std::uint32_t adapterIndex = 0; adapterIndex <= 64; adapterIndex++)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerateStatus = factory->EnumAdapters1(adapterIndex, &adapter);
        if (enumerateStatus == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        REQUIRE(adapterIndex < 64);
        REQUIRE(SUCCEEDED(enumerateStatus));
        const AdapterFingerprint fingerprint = GetAdapterFingerprint(adapter.Get(), "hardware", adapterIndex, false);
        if ((fingerprint.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        HardwareAdapterCandidate candidate;
        candidate.adapter = adapter;
        candidate.fingerprint = fingerprint;
        D3DEnvironment probe;
        candidate.deviceCreateStatus = CreateHardwareEnvironment(adapter.Get(), probe);
        if (SUCCEEDED(candidate.deviceCreateStatus))
        {
            candidate.featureLevel = probe.featureLevel;
        }
        candidates.push_back(std::move(candidate));
    }
    REQUIRE(factory->IsCurrent() != FALSE);
    return candidates;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    REQUIRE(value.size() <= static_cast<std::size_t>(INT_MAX));
    const int length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string output(static_cast<std::size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        output.data(), required, nullptr, nullptr) == required);
    return output;
}

[[nodiscard]] std::string JsonEscape(const std::string_view value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        }
        else if (character < 0x20)
        {
            output.append("\\u00");
            output.push_back(kHexDigits[character >> 4]);
            output.push_back(kHexDigits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    return output;
}

[[nodiscard]] std::string HexBytes(const std::span<const std::byte> bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned value = std::to_integer<unsigned>(bytes[index]);
        output[index * 2] = kHexDigits[(value >> 4) & 0x0F];
        output[index * 2 + 1] = kHexDigits[value & 0x0F];
    }
    return output;
}

[[nodiscard]] std::string Hex32(const std::uint32_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

[[nodiscard]] std::string AdapterJson(const AdapterFingerprint& fingerprint)
{
    const std::wstring_view wideDescription(fingerprint.description.Description);
    std::ostringstream output;
    output << "{\"backend\":\"" << JsonEscape(fingerprint.backend) << "\",\"adapterIndex\":";
    if (fingerprint.adapterIndex == UINT32_MAX)
    {
        output << "null";
    }
    else
    {
        output << fingerprint.adapterIndex;
    }
    output << ",\"description\":\"" << JsonEscape(WideToUtf8(wideDescription)) << "\""
           << ",\"softwareRasterizer\":" << (fingerprint.softwareRasterizer ? "true" : "false")
           << ",\"vendorId\":" << fingerprint.description.VendorId
           << ",\"deviceId\":" << fingerprint.description.DeviceId
           << ",\"luidLow\":\"" << Hex32(fingerprint.description.AdapterLuid.LowPart) << "\""
           << ",\"luidHigh\":\"" << Hex32(static_cast<std::uint32_t>(
                fingerprint.description.AdapterLuid.HighPart)) << "\""
           << ",\"driverVersionRaw\":";
    if (fingerprint.driverVersionAvailable)
    {
        output << '"' << fingerprint.driverVersion << '"';
    }
    else
    {
        output << "null";
    }
    output << '}';
    return output.str();
}

void WriteJsonLine(std::ofstream& output, const std::string& line)
{
    output << line << '\n';
    REQUIRE(output);
}

[[nodiscard]] ComPtr<ID3D11Texture2D> UploadBgraTexture(ID3D11Device* const device,
    const std::span<const std::byte> pixels, const std::uint32_t width, const std::uint32_t height,
    const std::size_t rowPitch)
{
    REQUIRE(device != nullptr);
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE(rowPitch >= static_cast<std::size_t>(width) * 4);
    REQUIRE(rowPitch <= UINT32_MAX);
    REQUIRE(pixels.size() >= rowPitch * height);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), static_cast<UINT>(rowPitch), 0};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)));
    return texture;
}

[[nodiscard]] pbcapturenormalize::ScreenCaptureFrame MakeFrame(ID3D11Texture2D* const texture,
    const LUID adapterLuid, const pbcapturenormalize::ScreenCaptureDomain& domain,
    const std::uint64_t observation)
{
    REQUIRE(texture != nullptr);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    REQUIRE(description.Width <= INT32_MAX);
    REQUIRE(description.Height <= INT32_MAX);
    const std::int32_t width = static_cast<std::int32_t>(description.Width);
    const std::int32_t height = static_cast<std::int32_t>(description.Height);
    pbcapturenormalize::ScreenCaptureFrame frame;
    frame.texture = texture;
    frame.metadata.domain = domain;
    frame.metadata.captureObservation = observation;
    frame.metadata.sourceGeneration = 1;
    frame.metadata.slotGeneration = observation;
    frame.metadata.slotIndex = static_cast<std::uint32_t>(observation % 2);
    frame.metadata.physicalRoi = {0, 0, width, height};
    frame.metadata.sourceContentSize = {width, height};
    frame.metadata.sourceExtent = {width, height};
    frame.metadata.roiSize = {width, height};
    frame.metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame.metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame.metadata.adapterLuid = adapterLuid;
    frame.metadata.bitsPerColor = 8;
    frame.metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    frame.metadata.isCursorExcluded = true;
    frame.metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return frame;
}

[[nodiscard]] pbdemodd3d11::DemodFrameResult PollUntilReady(pbdemodd3d11::Demodulator& demodulator,
    ID3D11DeviceContext* const context, const pbdemodd3d11::DemodSubmission& submission)
{
    pbdemodd3d11::DemodFrameResult result;
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    for (;;)
    {
        const pbdemodd3d11::DemodPollResult poll = demodulator.Poll(context, submission, result);
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

[[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
    const std::uint64_t sequence, const pbprotocol::SessionTag sessionTag)
{
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion,
        pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId, sessionTag, sequence,
        0x47100000U | static_cast<std::uint32_t>(sequence & 0xFFFFU), 0};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

[[nodiscard]] std::array<std::byte, pbmodulation::kUnifiedInformationBytes> MakeCorpusTransportBlock(
    const std::uint64_t sequence, const std::uint32_t slot)
{
    std::array<std::byte, kTransportPayloadBytes> payload{};
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((sequence * 29 + slot * 71 + index * 17 + index / 7) & 0xFFU);
    }
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, unifiedtransformtest::kCorpusSessionTag, 10000 + sequence,
        static_cast<std::uint32_t>(sequence * pbmodulation::kUnifiedCodewordCount + slot),
        static_cast<std::uint16_t>(payload.size())};
    std::array<std::byte, pbmodulation::kUnifiedInformationBytes> block{};
    REQUIRE(pbprotocol::GetTransportSerializedSize(header) == block.size());
    REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, block));
    return block;
}

struct CorpusFrame
{
    std::uint64_t sequence = 0;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, pbmodulation::kUnifiedCodedFrameBytes> coded{};
    std::array<std::array<std::byte, pbmodulation::kUnifiedInformationBytes>,
        pbmodulation::kUnifiedCodewordCount> transportBlocks{};
    std::vector<std::byte> pixels;
};

[[nodiscard]] CorpusFrame BuildCorpusFrame(const std::uint64_t sequence)
{
    CorpusFrame fixture;
    fixture.sequence = sequence;
    fixture.bootstrap = MakeBootstrap(sequence, unifiedtransformtest::kCorpusSessionTag);
    for (std::uint32_t slot = 0; slot < pbmodulation::kUnifiedCodewordCount; slot++)
    {
        fixture.transportBlocks[slot] = MakeCorpusTransportBlock(sequence, slot);
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust,
            fixture.transportBlocks[slot], std::span(fixture.coded).subspan(
                static_cast<std::size_t>(slot) * pbmodulation::kUnifiedCodewordBytes,
                pbmodulation::kUnifiedCodewordBytes)));
    }
    fixture.pixels.resize(pbmodulation::kUnifiedFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, fixture.pixels));
    return fixture;
}

[[nodiscard]] pbremotevisualsimulator::BgraImageView MakeCorpusView(const CorpusFrame& frame)
{
    return {frame.pixels, pbmodulation::kUnifiedVisualProfile.canvasWidth,
        pbmodulation::kUnifiedVisualProfile.canvasHeight,
        static_cast<std::size_t>(pbmodulation::kUnifiedVisualProfile.canvasWidth) * 4};
}

[[nodiscard]] pbmodulation::LumaView MakeLumaView(const pbremotevisualsimulator::BgraImage& image)
{
    return {image.pixels, image.width, image.height, image.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
}

[[nodiscard]] std::array<std::byte, 32> AcceptedSetDigest(
    const std::span<const pbmodulation::UnifiedAcceptedBlock> blocks)
{
    pbprotocol::Blake3Hasher hasher;
    for (const pbmodulation::UnifiedAcceptedBlock& block : blocks)
    {
        REQUIRE(block.size <= block.bytes.size());
        const std::array<std::byte, 6> prefix{
            static_cast<std::byte>(block.codewordSlot), static_cast<std::byte>(block.kind),
            static_cast<std::byte>(block.size & 0xFFU), static_cast<std::byte>((block.size >> 8U) & 0xFFU),
            static_cast<std::byte>((block.size >> 16U) & 0xFFU), static_cast<std::byte>((block.size >> 24U) & 0xFFU)};
        hasher.Update(prefix);
        hasher.Update(std::span(block.bytes).first(block.size));
    }
    return hasher.Finalize();
}

[[nodiscard]] std::uint32_t CountCorpusTruthMismatches(
    const std::span<const pbmodulation::UnifiedAcceptedBlock> blocks,
    const std::array<std::array<std::byte, pbmodulation::kUnifiedInformationBytes>,
        pbmodulation::kUnifiedCodewordCount>& expected)
{
    std::array<bool, pbmodulation::kUnifiedCodewordCount> seen{};
    std::uint32_t mismatches = 0;
    for (const pbmodulation::UnifiedAcceptedBlock& block : blocks)
    {
        if (block.codewordSlot >= expected.size() || seen[block.codewordSlot] ||
            block.kind != pbmodulation::UnifiedSlotKind::Transport || block.size != expected[block.codewordSlot].size())
        {
            mismatches++;
            continue;
        }
        seen[block.codewordSlot] = true;
        if (!std::ranges::equal(std::span(block.bytes).first(block.size), expected[block.codewordSlot]))
        {
            mismatches++;
        }
    }
    return mismatches;
}

void RequireSameObservation(const pbmodulation::UnifiedVisualObservation& cpu,
    const pbmodulation::UnifiedVisualObservation& gpu)
{
    REQUIRE(gpu.inputValid == cpu.inputValid);
    REQUIRE(gpu.frameErasure == cpu.frameErasure);
    REQUIRE(gpu.bootstrap.erasure == cpu.bootstrap.erasure);
    REQUIRE(gpu.bootstrap.canonical44 == cpu.bootstrap.canonical44);
    REQUIRE(gpu.bootstrapRecord == cpu.bootstrapRecord);
    REQUIRE(gpu.baseLuma == cpu.baseLuma);
    REQUIRE(gpu.fineLuma == cpu.fineLuma);
    REQUIRE(gpu.chroma == cpu.chroma);
    for (std::size_t region = 0; region < cpu.freshness.size(); region++)
    {
        REQUIRE(gpu.freshness[region].current == cpu.freshness[region].current);
        REQUIRE(gpu.freshness[region].bitErrors == cpu.freshness[region].bitErrors);
        REQUIRE(gpu.freshness[region].residual == Catch::Approx(cpu.freshness[region].residual).margin(1e-5));
    }
    for (std::size_t slot = 0; slot < cpu.slots.size(); slot++)
    {
        REQUIRE(gpu.slots[slot].lane == cpu.slots[slot].lane);
        REQUIRE(gpu.slots[slot].kind == cpu.slots[slot].kind);
        REQUIRE(gpu.slots[slot].rejection == cpu.slots[slot].rejection);
        REQUIRE(gpu.slots[slot].acceptedBytes == cpu.slots[slot].acceptedBytes);
        REQUIRE(gpu.slots[slot].fecValid == cpu.slots[slot].fecValid);
        REQUIRE(gpu.slots[slot].paddingValid == cpu.slots[slot].paddingValid);
        REQUIRE(gpu.slots[slot].crcValid == cpu.slots[slot].crcValid);
        REQUIRE(gpu.slots[slot].identityValid == cpu.slots[slot].identityValid);
        REQUIRE(gpu.slots[slot].accepted == cpu.slots[slot].accepted);
    }
    REQUIRE(gpu.acceptedBlocks == cpu.acceptedBlocks);
    REQUIRE(gpu.acceptedTransportBlocks == cpu.acceptedTransportBlocks);
    REQUIRE(gpu.acceptedControlRecords == cpu.acceptedControlRecords);
    REQUIRE(gpu.falseAcceptedBlocks == cpu.falseAcceptedBlocks);
}

void RequireSameAcceptedBlocks(const std::span<const pbmodulation::UnifiedAcceptedBlock> cpu,
    const std::span<const pbmodulation::UnifiedAcceptedBlock> gpu)
{
    REQUIRE(gpu.size() == cpu.size());
    for (std::size_t index = 0; index < cpu.size(); index++)
    {
        REQUIRE(gpu[index].codewordSlot == cpu[index].codewordSlot);
        REQUIRE(gpu[index].kind == cpu[index].kind);
        REQUIRE(gpu[index].size == cpu[index].size);
        REQUIRE(std::ranges::equal(std::span(gpu[index].bytes).first(gpu[index].size),
            std::span(cpu[index].bytes).first(cpu[index].size)));
    }
}

[[nodiscard]] bool MatchesExpectedIdentity(const pbprotocol::BootstrapRecord& record,
    const pbmodulation::UnifiedExpectedFrameIdentity& expected) noexcept
{
    return (!expected.requireSessionTag || record.sessionTag == expected.sessionTag) &&
        (!expected.requireFrameSequence || record.frameSequence == expected.frameSequence);
}

struct PreparedMandatoryCase
{
    std::string name;
    pbremotevisualsimulator::BgraImage image;
    pbmodulation::LocalDesktopObservation bootstrap;
    pbmodulation::UnifiedVisualObservation cpuObservation;
    std::vector<pbmodulation::UnifiedAcceptedBlock> cpuAccepted;
    std::array<std::array<std::byte, pbmodulation::kUnifiedInformationBytes>,
        pbmodulation::kUnifiedCodewordCount> expectedBlocks{};
    pbmodulation::UnifiedExpectedFrameIdentity expectedIdentity;
    bool conflictExpected = false;
    std::optional<pbmodulation::VisualIdentityDisposition> expectedTemporalDisposition;
    std::string channelManifestBlake3;
};

[[nodiscard]] std::vector<PreparedMandatoryCase> PrepareMandatoryCases()
{
    using namespace pbmodulation;
    using namespace pbremotevisualsimulator;

    auto oracleResult = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(oracleResult);
    UnifiedVisualCpuOracle oracle = std::move(oracleResult).Value();
    const CorpusFrame frame40 = BuildCorpusFrame(40);
    const CorpusFrame frame41 = BuildCorpusFrame(41);
    const std::vector<unifiedtransformtest::MandatoryTransformCase> transformCases =
        unifiedtransformtest::MakeMandatoryTransformCases();
    std::vector<PreparedMandatoryCase> preparedCases;
    preparedCases.reserve(transformCases.size());
    VisualIdentityTracker temporalTracker;
    std::size_t temporalObservationIndex = 0;
    for (const unifiedtransformtest::MandatoryTransformCase& transformCase : transformCases)
    {
        std::optional<CorpusFrame> dynamicFrame;
        const CorpusFrame* source = nullptr;
        if (transformCase.sequence == frame40.sequence)
        {
            source = &frame40;
        }
        else if (transformCase.sequence == frame41.sequence)
        {
            source = &frame41;
        }
        else
        {
            dynamicFrame.emplace(BuildCorpusFrame(transformCase.sequence));
            source = &*dynamicFrame;
        }
        const bool invalidReference = transformCase.referenceSequence.has_value() &&
            *transformCase.referenceSequence != frame40.sequence;
        REQUIRE_FALSE(invalidReference);
        const std::optional<BgraImageView> reference = transformCase.referenceSequence ?
            std::optional<BgraImageView>{MakeCorpusView(frame40)} : std::nullopt;
        auto executionResult = ExecuteChannelTransformPlan(MakeCorpusView(*source), reference,
            {unifiedtransformtest::MakeCorpusSeed(transformCase.name, source->sequence), transformCase.transforms});
        INFO(transformCase.name);
        REQUIRE(executionResult);
        ChannelTransformExecution execution = std::move(executionResult).Value();
        const LumaView view = MakeLumaView(execution.output);
        const LocalDesktopBootstrapBinding binding{kUnifiedVisualProfile.productProfile.visualProfileId,
            kUnifiedVisualProfile.productProfile.visualLayoutVersion};
        PreparedMandatoryCase prepared;
        prepared.name = transformCase.name;
        prepared.bootstrap = DecodeLocalDesktopBootstrap(view, binding);
        prepared.cpuObservation = oracle.DecodeMixedFrame(view, transformCase.expectedIdentity);
        prepared.cpuAccepted.assign(oracle.GetAcceptedBlocks().begin(), oracle.GetAcceptedBlocks().end());
        prepared.expectedBlocks = source->transportBlocks;
        prepared.expectedIdentity = transformCase.expectedIdentity;
        prepared.conflictExpected = transformCase.conflictExpected;
        prepared.expectedTemporalDisposition = transformCase.expectedTemporalDisposition;
        prepared.channelManifestBlake3 = ChannelDigestToHex(execution.manifestBlake3);
        prepared.image = std::move(execution.output);
        REQUIRE(prepared.cpuObservation.falseAcceptedBlocks == 0);
        REQUIRE(CountCorpusTruthMismatches(prepared.cpuAccepted, prepared.expectedBlocks) == 0);
        if (prepared.conflictExpected)
        {
            REQUIRE(prepared.cpuAccepted.empty());
        }
        if (prepared.expectedTemporalDisposition)
        {
            const VisualIdentityDisposition disposition = temporalTracker.Observe(source->sequence,
                unifiedtransformtest::kCorpusCaptureEpoch,
                static_cast<std::int64_t>(temporalObservationIndex + 1) * 10000000,
                unifiedtransformtest::kCorpusSessionTag.value);
            REQUIRE(disposition == *prepared.expectedTemporalDisposition);
            temporalObservationIndex++;
        }
        preparedCases.push_back(std::move(prepared));
    }
    REQUIRE(preparedCases.size() == unifiedtransformtest::kMandatoryTransformCaseCount);
    const VisualIdentitySnapshot snapshot = temporalTracker.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 3);
    REQUIRE(snapshot.duplicateFrames == 1);
    REQUIRE(snapshot.reorderedFrames == 1);
    REQUIRE(snapshot.gapEvents == 2);
    REQUIRE(snapshot.skippedSequences == 2);
    return preparedCases;
}

struct MandatoryParityRunSummary
{
    std::uint32_t scenarioCount = 0;
    std::uint32_t gpuSubmittedCount = 0;
    std::uint64_t acceptedBlocks = 0;
    std::uint64_t falseAcceptedBlocks = 0;
    std::uint64_t truthMismatchedBlocks = 0;
    std::uint64_t conflictOutputBlocks = 0;
};

[[nodiscard]] MandatoryParityRunSummary RunMandatoryParityCorpus(D3DEnvironment& environment,
    const AdapterFingerprint& fingerprint, const std::span<const PreparedMandatoryCase> preparedCases,
    std::ofstream& report)
{
    pbdemodd3d11::DemodConfig config;
    config.readbackSlotCount = 2;
    std::uint64_t residentBytes = 0;
    REQUIRE(pbdemodd3d11::CalculateDemodulatorResidentBytes(config, residentBytes));
    config.maximumResidentBytes = residentBytes;
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, demodulator));

    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x12};
    domain.sourceId[1] = static_cast<std::byte>(fingerprint.description.VendorId & 0xFFU);
    domain.sourceId[2] = static_cast<std::byte>(fingerprint.description.DeviceId & 0xFFU);
    domain.captureEpoch = 1200;
    pbmodulation::VisualIdentityTracker temporalTracker;
    std::size_t temporalObservationIndex = 0;
    MandatoryParityRunSummary summary;
    summary.scenarioCount = static_cast<std::uint32_t>(preparedCases.size());
    std::uint64_t observation = 1;
    for (const PreparedMandatoryCase& prepared : preparedCases)
    {
        INFO(prepared.name);
        bool gpuSubmitted = false;
        std::span<const pbmodulation::UnifiedAcceptedBlock> gpuAccepted;
        std::optional<pbdemodd3d11::DemodFrameResult> gpuResult;
        const bool identityValid = prepared.bootstrap.IsAccepted() &&
            MatchesExpectedIdentity(prepared.cpuObservation.bootstrapRecord, prepared.expectedIdentity);
        if (identityValid)
        {
            const ComPtr<ID3D11Texture2D> texture = UploadBgraTexture(environment.device.Get(),
                prepared.image.pixels, prepared.image.width, prepared.image.height, prepared.image.rowPitch);
            const pbcapturenormalize::ScreenCaptureFrame frame = MakeFrame(
                texture.Get(), environment.adapterLuid, domain, observation);
            pbdemodd3d11::DemodSubmission submission;
            REQUIRE(demodulator->SubmitUnifiedVisual(frame, environment.context.Get(), prepared.bootstrap, {}, submission));
            gpuResult.emplace(PollUntilReady(*demodulator, environment.context.Get(), submission));
            RequireSameObservation(prepared.cpuObservation, gpuResult->unifiedObservation);
            gpuAccepted = std::span(gpuResult->acceptedUnifiedBlocks).first(gpuResult->acceptedUnifiedBlockCount);
            RequireSameAcceptedBlocks(prepared.cpuAccepted, gpuAccepted);
            REQUIRE(gpuResult->unifiedObservation.falseAcceptedBlocks == 0);
            REQUIRE(gpuResult->remoteMetricSummaryAvailable);
            REQUIRE(gpuResult->remoteMetricSamples == pbmodulation::kUnifiedSoftMetricCount);
            gpuSubmitted = true;
            summary.gpuSubmittedCount++;
        }
        else
        {
            REQUIRE(prepared.cpuAccepted.empty());
            REQUIRE(prepared.cpuObservation.acceptedBlocks == 0);
            REQUIRE_FALSE(prepared.cpuObservation.IsFrameAvailable());
        }

        const std::uint32_t truthMismatches = CountCorpusTruthMismatches(
            gpuSubmitted ? gpuAccepted : std::span<const pbmodulation::UnifiedAcceptedBlock>{},
            prepared.expectedBlocks);
        REQUIRE(truthMismatches == 0);
        summary.truthMismatchedBlocks += truthMismatches;
        summary.falseAcceptedBlocks += prepared.cpuObservation.falseAcceptedBlocks;
        summary.acceptedBlocks += prepared.cpuAccepted.size();
        if (prepared.conflictExpected)
        {
            summary.conflictOutputBlocks += prepared.cpuAccepted.size();
            REQUIRE(prepared.cpuAccepted.empty());
        }
        if (prepared.expectedTemporalDisposition)
        {
            REQUIRE(gpuSubmitted);
            const pbmodulation::VisualIdentityDisposition disposition = temporalTracker.Observe(
                prepared.cpuObservation.bootstrapRecord.frameSequence, domain.captureEpoch,
                static_cast<std::int64_t>(temporalObservationIndex + 1) * 10000000,
                prepared.cpuObservation.bootstrapRecord.sessionTag.value);
            REQUIRE(disposition == *prepared.expectedTemporalDisposition);
            temporalObservationIndex++;
        }

        const std::array<std::byte, 32> cpuDigest = AcceptedSetDigest(prepared.cpuAccepted);
        const std::array<std::byte, 32> gpuDigest = AcceptedSetDigest(
            gpuSubmitted ? gpuAccepted : std::span<const pbmodulation::UnifiedAcceptedBlock>{});
        REQUIRE(cpuDigest == gpuDigest);
        std::ostringstream line;
        line << "{\"schema\":\"PixelBridge.UnifiedGpuParity.Record.1\",\"adapter\":"
             << AdapterJson(fingerprint) << ",\"scenario\":\"" << JsonEscape(prepared.name) << "\""
             << ",\"channelManifestBlake3\":\"" << prepared.channelManifestBlake3 << "\""
             << ",\"gpuSubmitted\":" << (gpuSubmitted ? "true" : "false")
             << ",\"preGpuAdmission\":\"" << (gpuSubmitted ? "accepted" : "rejected") << "\""
             << ",\"frameErasure\":" << static_cast<std::uint32_t>(prepared.cpuObservation.frameErasure)
             << ",\"laneErasure\":[" << static_cast<std::uint32_t>(prepared.cpuObservation.baseLuma.erasureReason)
             << ',' << static_cast<std::uint32_t>(prepared.cpuObservation.fineLuma.erasureReason)
             << ',' << static_cast<std::uint32_t>(prepared.cpuObservation.chroma.erasureReason) << ']'
             << ",\"acceptedBlocks\":" << prepared.cpuAccepted.size()
             << ",\"acceptedSetBlake3\":\"" << HexBytes(cpuDigest) << "\""
             << ",\"falseAcceptedBlocks\":" << prepared.cpuObservation.falseAcceptedBlocks
             << ",\"truthMismatchedBlocks\":" << truthMismatches
             << ",\"conflictExpected\":" << (prepared.conflictExpected ? "true" : "false") << '}';
        WriteJsonLine(report, line.str());
        observation++;
    }

    const pbmodulation::VisualIdentitySnapshot temporalSnapshot = temporalTracker.GetSnapshot();
    REQUIRE(temporalSnapshot.uniqueFrames == 3);
    REQUIRE(temporalSnapshot.duplicateFrames == 1);
    REQUIRE(temporalSnapshot.reorderedFrames == 1);
    REQUIRE(temporalSnapshot.gapEvents == 2);
    REQUIRE(temporalSnapshot.skippedSequences == 2);
    const pbdemodd3d11::DemodSnapshot snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == summary.gpuSubmittedCount);
    REQUIRE(snapshot.completedFrames == summary.gpuSubmittedCount);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.failedFrames == 0);
    REQUIRE(snapshot.cancelledFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(environment.device->GetDeviceRemovedReason() == S_OK);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
    REQUIRE(summary.falseAcceptedBlocks == 0);
    REQUIRE(summary.truthMismatchedBlocks == 0);
    REQUIRE(summary.conflictOutputBlocks == 0);
    std::ostringstream line;
    line << "{\"schema\":\"PixelBridge.UnifiedGpuParity.Run.1\",\"adapter\":"
         << AdapterJson(fingerprint) << ",\"featureLevel\":" << static_cast<std::uint32_t>(environment.featureLevel)
         << ",\"scenarios\":" << summary.scenarioCount
         << ",\"gpuSubmittedScenarios\":" << summary.gpuSubmittedCount
         << ",\"acceptedBlocks\":" << summary.acceptedBlocks
         << ",\"falseAcceptedBlocks\":" << summary.falseAcceptedBlocks
         << ",\"truthMismatchedBlocks\":" << summary.truthMismatchedBlocks
         << ",\"conflictOutputBlocks\":" << summary.conflictOutputBlocks
         << ",\"metricReadbackBytes\":" << snapshot.metricReadbackBytes
         << ",\"rawPixelReadbackBytes\":" << snapshot.rawPixelReadbackBytes
         << ",\"deviceRemovedHresult\":\"0x00000000\",\"shutdownCompleted\":true}";
    WriteJsonLine(report, line.str());
    return summary;
}

[[nodiscard]] std::vector<std::byte> MakeSmallTransport(const pbprotocol::SessionTag sessionTag,
    const std::uint32_t slot)
{
    std::vector<std::byte> payload(48 + slot % 13);
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((slot * 37 + index * 11 + 5) & 0xFFU);
    }
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, sessionTag, 900 + slot, 1000 + slot,
        static_cast<std::uint16_t>(payload.size())};
    std::vector<std::byte> block(pbprotocol::GetTransportSerializedSize(header));
    REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, block));
    return block;
}

[[nodiscard]] std::vector<std::byte> MakeSmallControl(const pbprotocol::SessionTag sessionTag)
{
    const std::array<std::byte, 9> payload{std::byte{1}, std::byte{3}, std::byte{5}, std::byte{7},
        std::byte{9}, std::byte{11}, std::byte{13}, std::byte{15}, std::byte{17}};
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, 77, sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

struct MixedSemanticFixture
{
    pbprotocol::SessionTag sessionTag{};
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
    std::array<std::vector<std::byte>, pbmodulation::kUnifiedCodewordCount> blocks;
    std::array<std::byte, pbmodulation::kUnifiedCodedFrameBytes> coded{};
    std::vector<std::byte> pixels;
};

[[nodiscard]] MixedSemanticFixture BuildMixedSemanticFixture()
{
    MixedSemanticFixture fixture;
    fixture.sessionTag = {0x1122334455667788ULL};
    fixture.bootstrap = MakeBootstrap(70, fixture.sessionTag);
    fixture.blocks[0] = MakeSmallControl(fixture.sessionTag);
    for (std::uint32_t slot = 1; slot < pbmodulation::kUnifiedCodewordCount; slot++)
    {
        fixture.blocks[slot] = MakeSmallTransport(fixture.sessionTag, slot);
    }
    std::array<pbmodulation::UnifiedFrameSlotInput, pbmodulation::kUnifiedCodewordCount> inputs{};
    for (std::uint32_t slot = 0; slot < inputs.size(); slot++)
    {
        inputs[slot].assignment = {slot, slot == 0 ? pbmodulation::UnifiedSlotKind::Control :
            pbmodulation::UnifiedSlotKind::Transport, slot == 0 ?
            pbmodulation::UnifiedControlPriority::SessionDescriptor :
            pbmodulation::UnifiedControlPriority::NotApplicable};
        inputs[slot].active = true;
        inputs[slot].block = fixture.blocks[slot];
    }
    REQUIRE(pbmodulation::PackUnifiedVisualFrame({fixture.bootstrap, inputs}, fixture.coded));
    fixture.pixels.resize(pbmodulation::kUnifiedFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, fixture.pixels));
    return fixture;
}

void ReencodeSlot(std::array<std::byte, pbmodulation::kUnifiedCodedFrameBytes>& coded,
    const std::uint32_t slot)
{
    std::array<std::byte, pbmodulation::kUnifiedInformationBytes> information{};
    const std::size_t offset = static_cast<std::size_t>(slot) * pbmodulation::kUnifiedCodewordBytes;
    std::copy_n(coded.begin() + static_cast<std::ptrdiff_t>(offset), information.size(), information.begin());
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
        std::span(coded).subspan(offset, pbmodulation::kUnifiedCodewordBytes)));
}

[[nodiscard]] std::uint32_t CountMixedTruthMismatches(
    const std::span<const pbmodulation::UnifiedAcceptedBlock> accepted,
    const MixedSemanticFixture& fixture)
{
    std::array<bool, pbmodulation::kUnifiedCodewordCount> seen{};
    std::uint32_t mismatches = 0;
    for (const pbmodulation::UnifiedAcceptedBlock& block : accepted)
    {
        if (block.codewordSlot >= fixture.blocks.size() || seen[block.codewordSlot] ||
            block.kind != (block.codewordSlot == 0 ? pbmodulation::UnifiedSlotKind::Control :
                pbmodulation::UnifiedSlotKind::Transport) || block.size != fixture.blocks[block.codewordSlot].size())
        {
            mismatches++;
            continue;
        }
        seen[block.codewordSlot] = true;
        if (!std::ranges::equal(std::span(block.bytes).first(block.size), fixture.blocks[block.codewordSlot]))
        {
            mismatches++;
        }
    }
    return mismatches;
}

void RunMixedSemanticParity(D3DEnvironment& environment, const AdapterFingerprint& fingerprint,
    std::ofstream& report)
{
    const MixedSemanticFixture clean = BuildMixedSemanticFixture();
    std::array<std::byte, pbmodulation::kUnifiedCodedFrameBytes> damagedCoded = clean.coded;
    damagedCoded[26] ^= std::byte{1};
    ReencodeSlot(damagedCoded, 0);
    damagedCoded[static_cast<std::size_t>(1) * pbmodulation::kUnifiedCodewordBytes +
        pbmodulation::kUnifiedInformationBytes - 1] = std::byte{1};
    ReencodeSlot(damagedCoded, 1);
    damagedCoded[static_cast<std::size_t>(2) * pbmodulation::kUnifiedCodewordBytes +
        pbprotocol::kTransportPayloadOffset] ^= std::byte{1};
    ReencodeSlot(damagedCoded, 2);
    std::array<std::byte, pbmodulation::kUnifiedInformationBytes> wrongIdentityInformation{};
    const std::vector<std::byte> wrongIdentity = MakeSmallTransport({clean.sessionTag.value ^ 1ULL}, 3);
    REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(wrongIdentity,
        wrongIdentityInformation.size(), wrongIdentityInformation));
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, wrongIdentityInformation,
        std::span(damagedCoded).subspan(static_cast<std::size_t>(3) * pbmodulation::kUnifiedCodewordBytes,
            pbmodulation::kUnifiedCodewordBytes)));
    const std::size_t fecOffset = static_cast<std::size_t>(4) * pbmodulation::kUnifiedCodewordBytes;
    for (std::size_t index = 0; index < 512; index++)
    {
        damagedCoded[fecOffset + index] ^= static_cast<std::byte>((index * 73U + 0x5BU) & 0xFFU);
    }
    std::vector<std::byte> damagedPixels(pbmodulation::kUnifiedFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeUnifiedVisualFrame(clean.bootstrap, damagedCoded, damagedPixels));

    struct SemanticCase
    {
        std::string_view name;
        std::span<const std::byte> pixels;
        std::uint32_t expectedAcceptedBlocks = 0;
        bool damaged = false;
    };
    const std::array cases{
        SemanticCase{"mixed-control-clean", clean.pixels, pbmodulation::kUnifiedCodewordCount, false},
        SemanticCase{"mixed-post-fec-gates", damagedPixels, pbmodulation::kUnifiedCodewordCount - 5, true}};
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(
        pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(oracleResult);
    pbmodulation::UnifiedVisualCpuOracle oracle = std::move(oracleResult).Value();
    pbdemodd3d11::DemodConfig config;
    config.readbackSlotCount = 2;
    std::uint64_t residentBytes = 0;
    REQUIRE(pbdemodd3d11::CalculateDemodulatorResidentBytes(config, residentBytes));
    config.maximumResidentBytes = residentBytes;
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, demodulator));
    const pbcapturenormalize::ScreenCaptureDomain domain{{std::byte{0x5A}}, 1250};
    std::uint64_t observation = 1;
    for (const SemanticCase& semanticCase : cases)
    {
        INFO(semanticCase.name);
        const pbmodulation::LumaView view{semanticCase.pixels,
            pbmodulation::kUnifiedVisualProfile.canvasWidth, pbmodulation::kUnifiedVisualProfile.canvasHeight,
            static_cast<std::size_t>(pbmodulation::kUnifiedVisualProfile.canvasWidth) * 4,
            pbmodulation::LumaPixelFormat::Bgra8};
        const pbmodulation::LocalDesktopBootstrapBinding binding{
            pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId,
            pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion};
        const pbmodulation::LocalDesktopObservation bootstrap = pbmodulation::DecodeLocalDesktopBootstrap(view, binding);
        REQUIRE(bootstrap.IsAccepted());
        const pbmodulation::UnifiedVisualObservation cpuObservation = oracle.DecodeMixedFrame(view);
        const std::vector<pbmodulation::UnifiedAcceptedBlock> cpuAccepted(
            oracle.GetAcceptedBlocks().begin(), oracle.GetAcceptedBlocks().end());
        REQUIRE(cpuObservation.acceptedBlocks == semanticCase.expectedAcceptedBlocks);
        REQUIRE(cpuObservation.falseAcceptedBlocks == 0);
        REQUIRE(CountMixedTruthMismatches(cpuAccepted, clean) == 0);
        if (semanticCase.damaged)
        {
            REQUIRE(cpuObservation.slots[0].rejection == pbmodulation::UnifiedSlotRejection::ControlCrcFailure);
            REQUIRE(cpuObservation.slots[1].rejection == pbmodulation::UnifiedSlotRejection::NonCanonicalPadding);
            REQUIRE(cpuObservation.slots[2].rejection == pbmodulation::UnifiedSlotRejection::TransportCrcFailure);
            REQUIRE(cpuObservation.slots[3].rejection == pbmodulation::UnifiedSlotRejection::IdentityFailure);
            REQUIRE(cpuObservation.slots[4].rejection == pbmodulation::UnifiedSlotRejection::InnerFecFailure);
        }
        const ComPtr<ID3D11Texture2D> texture = UploadBgraTexture(environment.device.Get(), semanticCase.pixels,
            pbmodulation::kUnifiedVisualProfile.canvasWidth, pbmodulation::kUnifiedVisualProfile.canvasHeight,
            static_cast<std::size_t>(pbmodulation::kUnifiedVisualProfile.canvasWidth) * 4);
        const pbcapturenormalize::ScreenCaptureFrame frame = MakeFrame(
            texture.Get(), environment.adapterLuid, domain, observation);
        pbdemodd3d11::DemodSubmission submission;
        REQUIRE(demodulator->SubmitUnifiedVisual(frame, environment.context.Get(), bootstrap, {}, submission));
        const pbdemodd3d11::DemodFrameResult gpuResult = PollUntilReady(
            *demodulator, environment.context.Get(), submission);
        const std::span<const pbmodulation::UnifiedAcceptedBlock> gpuAccepted =
            std::span(gpuResult.acceptedUnifiedBlocks).first(gpuResult.acceptedUnifiedBlockCount);
        RequireSameObservation(cpuObservation, gpuResult.unifiedObservation);
        RequireSameAcceptedBlocks(cpuAccepted, gpuAccepted);
        REQUIRE(CountMixedTruthMismatches(gpuAccepted, clean) == 0);
        const std::array<std::byte, 32> acceptedDigest = AcceptedSetDigest(gpuAccepted);
        std::ostringstream line;
        line << "{\"schema\":\"PixelBridge.UnifiedGpuParity.SemanticGate.1\",\"adapter\":"
             << AdapterJson(fingerprint) << ",\"scenario\":\"" << semanticCase.name << "\""
             << ",\"acceptedBlocks\":" << gpuAccepted.size()
             << ",\"acceptedControlRecords\":" << gpuResult.unifiedObservation.acceptedControlRecords
             << ",\"acceptedTransportBlocks\":" << gpuResult.unifiedObservation.acceptedTransportBlocks
             << ",\"acceptedSetBlake3\":\"" << HexBytes(acceptedDigest) << "\""
             << ",\"falseAcceptedBlocks\":0,\"truthMismatchedBlocks\":0}";
        WriteJsonLine(report, line.str());
        observation++;
    }
    const pbdemodd3d11::DemodSnapshot snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == cases.size());
    REQUIRE(snapshot.completedFrames == cases.size());
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

class ScratchDirectory
{
public:
    explicit ScratchDirectory(const wchar_t* const name)
    {
        REQUIRE(name != nullptr);
        std::error_code error;
        const std::filesystem::path root = std::filesystem::absolute(
            std::filesystem::path(PB_G12_SCRATCH_ROOT), error).lexically_normal();
        REQUIRE_FALSE(error);
        REQUIRE(root.is_absolute());
        const std::filesystem::path candidate = (root / name).lexically_normal();
        REQUIRE(candidate.parent_path() == root);
        path_ = candidate;
        std::filesystem::remove_all(path_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void FillCsprng(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() <= ULONG_MAX);
    const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    REQUIRE(status == 0);
}

[[nodiscard]] pbprotocol::SessionId MakeCsprngSessionId()
{
    pbprotocol::SessionId sessionId;
    FillCsprng(sessionId.bytes);
    return sessionId;
}

[[nodiscard]] std::vector<std::byte> WrapControlPayload(const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion, recordType,
        controlSequence, sessionTag, payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(sizeResult);
    REQUIRE(sizeResult.Value() <= pbmodulation::kUnifiedInformationBytes);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionControlRecord(
    const pbprotocol::SessionDescriptor& descriptor, const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const auto serializedSize = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(serializedSize);
    std::vector<std::byte> payload(serializedSize.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(descriptor, resourcePolicy, payload));
    return WrapControlPayload(pbprotocol::ControlRecordType::SessionDescriptor, 1,
        pbprotocol::DeriveSessionTag(descriptor.sessionId), payload);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentControlRecord(
    const pbprotocol::SegmentDescriptor& segmentDescriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(segmentDescriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> payload(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(segmentDescriptor, sessionDescriptor, resourcePolicy, payload));
    return WrapControlPayload(pbprotocol::ControlRecordType::SegmentDescriptor, 3,
        segmentDescriptor.sessionTag, payload);
}

[[nodiscard]] std::vector<std::byte> MakeManifestControlRecord(const pbprotocol::FinalManifest& manifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(manifest, sessionDescriptor, resourcePolicy, payload));
    return WrapControlPayload(pbprotocol::ControlRecordType::FinalManifest, 2,
        pbprotocol::DeriveSessionTag(manifest.sessionId), payload);
}

struct EncodedEquation
{
    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    std::vector<std::byte> paddedPayload;
    std::vector<std::byte> wireBytes;
};

[[nodiscard]] EncodedEquation EncodeEquation(pbouterfec::WirehairV2Encoder& encoder,
    const std::uint32_t outerBlockId, const pbprotocol::SessionTag sessionTag)
{
    EncodedEquation equation;
    equation.outerBlockId = outerBlockId;
    equation.paddedPayload.resize(kTransportPayloadBytes);
    const auto encodeResult = encoder.EncodeBlock(outerBlockId, equation.paddedPayload);
    REQUIRE(encodeResult);
    REQUIRE(encodeResult.Value() > 0);
    REQUIRE(encodeResult.Value() <= equation.paddedPayload.size());
    REQUIRE(encodeResult.Value() <= UINT16_MAX);
    equation.declaredPayloadBytes = static_cast<std::uint16_t>(encodeResult.Value());
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, sessionTag, 0, outerBlockId, equation.declaredPayloadBytes};
    equation.wireBytes.resize(pbprotocol::GetTransportSerializedSize(header));
    REQUIRE(pbprotocol::SerializeTransportBlock(header,
        std::span(equation.paddedPayload).first(equation.declaredPayloadBytes), equation.wireBytes));
    return equation;
}

struct PerformanceControls
{
    std::vector<std::byte> session;
    std::vector<std::byte> manifest;
    std::vector<std::byte> segment;
};

struct PerformanceFrame
{
    pbremotevisualsimulator::BgraImage image;
    std::array<std::vector<std::byte>, pbmodulation::kUnifiedCodewordCount> blocks;
    std::array<bool, pbmodulation::kUnifiedCodewordCount> transportSlots{};
};

[[nodiscard]] PerformanceFrame BuildPerformanceFrame(pbouterfec::WirehairV2Encoder& encoder,
    const PerformanceControls& controls, const pbprotocol::SessionTag sessionTag,
    const std::uint64_t frameSequence, const bool baseOnlyUniqueEquations,
    std::uint32_t& nextUniqueEquationId)
{
    constexpr std::uint32_t initialControlSlotCount = 12;
    const std::uint32_t controlSlotCount = frameSequence == 1 ? initialControlSlotCount : 0;
    PerformanceFrame frame;
    std::array<pbmodulation::UnifiedFrameSlotInput, pbmodulation::kUnifiedCodewordCount> inputs{};
    std::vector<std::uint32_t> baseEquationIds;
    baseEquationIds.reserve(17);
    std::size_t duplicateIndex = 0;
    for (std::uint32_t slot = 0; slot < pbmodulation::kUnifiedCodewordCount; slot++)
    {
        pbmodulation::UnifiedFrameSlotInput& input = inputs[slot];
        input.assignment.codewordSlot = slot;
        if (slot < controlSlotCount)
        {
            input.assignment.kind = pbmodulation::UnifiedSlotKind::Control;
            if (slot < 4)
            {
                input.assignment.controlPriority = pbmodulation::UnifiedControlPriority::SessionDescriptor;
                frame.blocks[slot] = controls.session;
            }
            else if (slot < 8)
            {
                input.assignment.controlPriority = pbmodulation::UnifiedControlPriority::FinalManifest;
                frame.blocks[slot] = controls.manifest;
            }
            else
            {
                input.assignment.controlPriority = pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor;
                frame.blocks[slot] = controls.segment;
            }
            input.active = true;
            input.block = frame.blocks[slot];
            continue;
        }

        input.assignment.kind = pbmodulation::UnifiedSlotKind::Transport;
        input.assignment.controlPriority = pbmodulation::UnifiedControlPriority::NotApplicable;
        const pbmodulation::UnifiedLaneContract* const lane = pbmodulation::FindUnifiedLaneForCodewordSlot(slot);
        REQUIRE(lane != nullptr);
        std::uint32_t equationId = 0;
        if (!baseOnlyUniqueEquations || lane->lane == pbmodulation::UnifiedLane::BaseLuma)
        {
            equationId = nextUniqueEquationId;
            REQUIRE(nextUniqueEquationId < UINT32_MAX);
            nextUniqueEquationId++;
            if (lane->lane == pbmodulation::UnifiedLane::BaseLuma)
            {
                baseEquationIds.push_back(equationId);
            }
        }
        else
        {
            REQUIRE_FALSE(baseEquationIds.empty());
            equationId = baseEquationIds[duplicateIndex % baseEquationIds.size()];
            duplicateIndex++;
        }
        const EncodedEquation equation = EncodeEquation(encoder, equationId, sessionTag);
        frame.blocks[slot] = equation.wireBytes;
        frame.transportSlots[slot] = true;
        input.active = true;
        input.block = frame.blocks[slot];
    }

    std::vector<std::byte> canonicalPixels(pbmodulation::kUnifiedFrameBgraBytes);
    const std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap = MakeBootstrap(frameSequence, sessionTag);
    REQUIRE(pbmodulation::EncodeUnifiedVisualFrame({bootstrap, inputs}, canonicalPixels));
    if (baseOnlyUniqueEquations)
    {
        using namespace pbremotevisualsimulator;
        const std::array<std::byte, 4> matte{
            std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
        const std::array<ChannelTransform, 2> transforms{
            ResampleTransform{1920, 1080, 0.75, 0.75, 240, 135, ResampleFilter::Area, matte},
            NeutralChromaTransform{}};
        const BgraImageView source{canonicalPixels, pbmodulation::kUnifiedVisualProfile.canvasWidth,
            pbmodulation::kUnifiedVisualProfile.canvasHeight,
            static_cast<std::size_t>(pbmodulation::kUnifiedVisualProfile.canvasWidth) * 4};
        auto result = ExecuteChannelTransformPlan(source, std::nullopt,
            {0xB125000000000000ULL ^ frameSequence, transforms});
        REQUIRE(result);
        frame.image = std::move(result).Value().output;
    }
    else
    {
        frame.image.pixels = std::move(canonicalPixels);
        frame.image.width = pbmodulation::kUnifiedVisualProfile.canvasWidth;
        frame.image.height = pbmodulation::kUnifiedVisualProfile.canvasHeight;
        frame.image.rowPitch = static_cast<std::size_t>(frame.image.width) * 4;
    }
    return frame;
}

[[nodiscard]] std::vector<std::byte> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const std::streampos end = input.tellg();
    REQUIRE(end >= 0);
    REQUIRE(static_cast<std::uint64_t>(end) <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::streamsize>::max)()));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(input);
    return bytes;
}

struct PerformanceResult
{
    std::string name;
    std::uint64_t encodedBytes = 0;
    std::uint64_t uniqueFrames = 0;
    double verifiedEncodedBytesPerUniqueFrame = 0;
    std::uint64_t uniqueBaseEquations = 0;
    std::uint64_t uniqueNonBaseEquations = 0;
    bool hardThresholdPassed = false;
    bool cleanTargetPassed = false;
};

[[nodiscard]] PerformanceResult RunPublishedPerformanceCase(D3DEnvironment& environment,
    const AdapterFingerprint& fingerprint, const bool baseOnlyUniqueEquations, std::ofstream& report)
{
    constexpr std::size_t sourceBytes = 256ULL * 1024;
    constexpr std::uint64_t hardThresholdBytes = 16ULL * 1024;
    constexpr std::uint64_t cleanTargetBytes = 32ULL * 1024;
    constexpr std::uint64_t maximumLogicalFrames = 64;
    const std::string name = baseOnlyUniqueEquations ? "base-only-neutral-chroma-scale-075" : "clean-all-lanes";
    ScratchDirectory scratch(baseOnlyUniqueEquations ? L"g12-base-only-publish" : L"g12-clean-publish");
    std::vector<std::byte> source(sourceBytes);
    FillCsprng(source);
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor sessionDescriptor;
    sessionDescriptor.protocolVersion = pbprotocol::GetProtocolVersion();
    sessionDescriptor.sessionId = MakeCsprngSessionId();
    sessionDescriptor.originalFileSize = source.size();
    sessionDescriptor.segmentCount = 1;
    sessionDescriptor.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
    sessionDescriptor.sessionVisualProfileId = pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId;
    sessionDescriptor.fileNameUtf8 = baseOnlyUniqueEquations ? "g12-base-only.bin" : "g12-clean.bin";
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(source,
        static_cast<std::uint32_t>(kTransportPayloadBytes));
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    REQUIRE(encoder.GetBlockCount() == 200);
    pbprotocol::SegmentDescriptor segmentDescriptor;
    segmentDescriptor.sessionTag = sessionTag;
    segmentDescriptor.segmentOrdinal = 0;
    segmentDescriptor.rawOffset = 0;
    segmentDescriptor.rawSize = source.size();
    segmentDescriptor.encodedSize = source.size();
    segmentDescriptor.compressionCodec = pbprotocol::CompressionCodec::Raw;
    segmentDescriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    segmentDescriptor.outerBlockBytes = static_cast<std::uint32_t>(kTransportPayloadBytes);
    segmentDescriptor.rawDigest = pbprotocol::RawDigest{pbprotocol::ComputeBlake3Digest(source)};
    segmentDescriptor.encodedDigest = pbprotocol::EncodedDigest{pbprotocol::ComputeBlake3Digest(source)};
    segmentDescriptor.wirehairV2SerializedProfile = encoder.GetSerializedProfile();
    const pbprotocol::FinalManifest manifest{sessionDescriptor.sessionId, source.size(), 1,
        pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(source)},
        pbprotocol::DigestAlgorithm::Blake3_256};
    const PerformanceControls controls{MakeSessionControlRecord(sessionDescriptor, resourcePolicy),
        MakeManifestControlRecord(manifest, sessionDescriptor, resourcePolicy),
        MakeSegmentControlRecord(segmentDescriptor, sessionDescriptor, resourcePolicy)};

    auto receiverResult = pbreceiver::ReceiverIngress::Create(resourcePolicy,
        static_cast<std::uint32_t>(kTransportPayloadBytes));
    REQUIRE(receiverResult);
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    pbstorage::OutputFileConfig outputConfig;
    outputConfig.outputDirectory = scratch.GetPath().wstring();
    outputConfig.sessionTag = sessionTag;
    outputConfig.fileBytes = source.size();
    outputConfig.maximumFileBytes = resourcePolicy.maxAcceptedFileBytes;
    outputConfig.originalFileNameUtf8 = sessionDescriptor.fileNameUtf8;
    std::unique_ptr<pbstorage::OutputFile> outputFile;
    REQUIRE(pbstorage::OutputFile::Create(outputConfig, outputFile));

    pbdemodd3d11::DemodConfig demodConfig;
    demodConfig.readbackSlotCount = 2;
    std::uint64_t demodResidentBytes = 0;
    REQUIRE(pbdemodd3d11::CalculateDemodulatorResidentBytes(demodConfig, demodResidentBytes));
    demodConfig.maximumResidentBytes = demodResidentBytes;
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), demodConfig, demodulator));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = baseOnlyUniqueEquations ? std::byte{0xB1} : std::byte{0xC1};
    domain.captureEpoch = baseOnlyUniqueEquations ? 1301 : 1302;
    pbmodulation::VisualIdentityTracker identityTracker;
    std::optional<pbreceiver::ReceiverCompletedSegment> completedSegment;
    std::set<std::uint32_t> uniqueOuterBlockIds;
    std::uint64_t uniqueBaseEquations = 0;
    std::uint64_t uniqueNonBaseEquations = 0;
    std::uint64_t falseAcceptedBlocks = 0;
    std::uint64_t truthMismatchedBlocks = 0;
    std::uint32_t nextUniqueEquationId = 0;
    std::uint64_t frameSequence = 1;
    for (; frameSequence <= maximumLogicalFrames && !completedSegment; frameSequence++)
    {
        const PerformanceFrame performanceFrame = BuildPerformanceFrame(encoder, controls, sessionTag,
            frameSequence, baseOnlyUniqueEquations, nextUniqueEquationId);
        const pbmodulation::LumaView view = MakeLumaView(performanceFrame.image);
        const pbmodulation::LocalDesktopBootstrapBinding binding{
            pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId,
            pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion};
        const pbmodulation::LocalDesktopObservation bootstrap = pbmodulation::DecodeLocalDesktopBootstrap(view, binding);
        REQUIRE(bootstrap.IsAccepted());
        const ComPtr<ID3D11Texture2D> texture = UploadBgraTexture(environment.device.Get(),
            performanceFrame.image.pixels, performanceFrame.image.width, performanceFrame.image.height,
            performanceFrame.image.rowPitch);
        const pbcapturenormalize::ScreenCaptureFrame captureFrame = MakeFrame(
            texture.Get(), environment.adapterLuid, domain, frameSequence);
        pbdemodd3d11::DemodSubmission submission;
        REQUIRE(demodulator->SubmitUnifiedVisual(captureFrame, environment.context.Get(), bootstrap, {}, submission));
        const pbdemodd3d11::DemodFrameResult gpuResult = PollUntilReady(
            *demodulator, environment.context.Get(), submission);
        REQUIRE(gpuResult.unifiedObservation.IsFrameAvailable());
        REQUIRE(gpuResult.unifiedObservation.falseAcceptedBlocks == 0);
        falseAcceptedBlocks += gpuResult.unifiedObservation.falseAcceptedBlocks;
        if (baseOnlyUniqueEquations)
        {
            REQUIRE(gpuResult.unifiedObservation.baseLuma.erasureReason == pbmodulation::UnifiedErasureReason::None);
            REQUIRE(gpuResult.unifiedObservation.chroma.erasureReason ==
                pbmodulation::UnifiedErasureReason::ChromaPilotFailure);
        }
        else
        {
            REQUIRE(gpuResult.unifiedObservation.baseLuma.erasureReason == pbmodulation::UnifiedErasureReason::None);
            REQUIRE(gpuResult.unifiedObservation.fineLuma.erasureReason == pbmodulation::UnifiedErasureReason::None);
            REQUIRE(gpuResult.unifiedObservation.chroma.erasureReason == pbmodulation::UnifiedErasureReason::None);
        }
        const pbmodulation::VisualIdentityDisposition temporalDisposition = identityTracker.Observe(
            frameSequence, domain.captureEpoch, static_cast<std::int64_t>(frameSequence) * 10000000,
            sessionTag.value);
        REQUIRE(temporalDisposition == pbmodulation::VisualIdentityDisposition::Unique);
        const std::span<const pbmodulation::UnifiedAcceptedBlock> accepted =
            std::span(gpuResult.acceptedUnifiedBlocks).first(gpuResult.acceptedUnifiedBlockCount);
        for (const pbmodulation::UnifiedAcceptedBlock& block : accepted)
        {
            REQUIRE(block.codewordSlot < performanceFrame.blocks.size());
            const std::vector<std::byte>& expected = performanceFrame.blocks[block.codewordSlot];
            if (block.size != expected.size() ||
                !std::ranges::equal(std::span(block.bytes).first(block.size), expected))
            {
                truthMismatchedBlocks++;
                continue;
            }
            if (block.kind == pbmodulation::UnifiedSlotKind::Control)
            {
                REQUIRE(receiver.ReceiveControlRecord(std::span(block.bytes).first(block.size)));
                continue;
            }
            REQUIRE(block.kind == pbmodulation::UnifiedSlotKind::Transport);
            const auto parsed = pbprotocol::ParseTransportBlock(std::span(block.bytes).first(block.size));
            REQUIRE(parsed);
            std::vector<std::byte> paddedPayload(kTransportPayloadBytes);
            REQUIRE(parsed.Value().payload.size() <= paddedPayload.size());
            std::copy(parsed.Value().payload.begin(), parsed.Value().payload.end(), paddedPayload.begin());
            const pbreceiver::ReceivedTransportBlock received{parsed.Value().header.sessionTag,
                parsed.Value().header.segmentOrdinal, parsed.Value().header.outerBlockId,
                parsed.Value().header.payloadBytes, paddedPayload};
            auto admissionResult = receiver.ReceiveDataBlock(received, frameSequence);
            REQUIRE(admissionResult);
            pbreceiver::ReceiverDataAdmission admission = std::move(admissionResult).Value();
            if (admission.outerSymbolAdmission == pbreceiver::ReceiverOuterSymbolAdmission::Unique)
            {
                REQUIRE(uniqueOuterBlockIds.insert(parsed.Value().header.outerBlockId).second);
                const pbmodulation::UnifiedLaneContract* const lane =
                    pbmodulation::FindUnifiedLaneForCodewordSlot(block.codewordSlot);
                REQUIRE(lane != nullptr);
                if (lane->lane == pbmodulation::UnifiedLane::BaseLuma)
                {
                    uniqueBaseEquations++;
                }
                else
                {
                    uniqueNonBaseEquations++;
                }
            }
            if (admission.completedSegment)
            {
                completedSegment.emplace(std::move(*admission.completedSegment));
                break;
            }
        }
        REQUIRE(truthMismatchedBlocks == 0);
        if (frameSequence == 1)
        {
            REQUIRE(receiver.GetTelemetry().activeSessionCount == 1);
            REQUIRE(gpuResult.unifiedObservation.acceptedControlRecords == 12);
        }
    }
    REQUIRE(completedSegment.has_value());
    REQUIRE(falseAcceptedBlocks == 0);
    REQUIRE(truthMismatchedBlocks == 0);
    if (baseOnlyUniqueEquations)
    {
        REQUIRE(uniqueNonBaseEquations == 0);
        REQUIRE(uniqueBaseEquations >= encoder.GetBlockCount());
    }

    auto verifiedResult = receiver.VerifyRecoveredSegment(std::move(*completedSegment));
    REQUIRE(verifiedResult);
    pbreceiver::ReceiverVerifiedSegment verifiedSegment = std::move(verifiedResult).Value();
    REQUIRE(std::ranges::equal(verifiedSegment.GetRawBytes(), source));
    const pbprotocol::SegmentDescriptor& verifiedDescriptor =
        verifiedSegment.GetBoundSegmentDescriptor().GetDescriptor();
    REQUIRE(verifiedDescriptor.compressionCodec == pbprotocol::CompressionCodec::Raw);
    REQUIRE(verifiedDescriptor.encodedSize == source.size());
    REQUIRE(outputFile->WriteVerifiedSegment(verifiedDescriptor.rawOffset, verifiedSegment.GetRawBytes()));
    REQUIRE(outputFile->FlushVerifiedSegment());
    const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedSegment));
    REQUIRE(commitResult);
    REQUIRE(commitResult.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed);
    const auto finalizationResult = receiver.PrepareFinalization(sessionTag);
    REQUIRE(finalizationResult);
    REQUIRE(finalizationResult.Value() == manifest);
    REQUIRE(outputFile->Publish(finalizationResult.Value().wholeFileDigest));
    const pbstorage::OutputFileSnapshot published = outputFile->GetSnapshot();
    REQUIRE(published.published);
    REQUIRE_FALSE(std::filesystem::exists(published.partPath));
    REQUIRE(std::filesystem::exists(published.finalPath));
    const std::vector<std::byte> reopened = ReadAllBytes(published.finalPath);
    REQUIRE(reopened == source);
    REQUIRE(pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(reopened)} == manifest.wholeFileDigest);

    const pbmodulation::VisualIdentitySnapshot identitySnapshot = identityTracker.GetSnapshot();
    REQUIRE(identitySnapshot.uniqueFrames > 0);
    REQUIRE(identitySnapshot.uniqueFrames <= maximumLogicalFrames);
    REQUIRE(identitySnapshot.duplicateFrames == 0);
    REQUIRE(identitySnapshot.reorderedFrames == 0);
    const std::uint64_t uniqueFrames = identitySnapshot.uniqueFrames;
    const std::uint64_t encodedBytes = segmentDescriptor.encodedSize;
    const auto hardProduct = pbprotocol::CheckedMultiplyUint64(uniqueFrames, hardThresholdBytes);
    const auto cleanProduct = pbprotocol::CheckedMultiplyUint64(uniqueFrames, cleanTargetBytes);
    REQUIRE(hardProduct);
    REQUIRE(cleanProduct);
    const bool hardThresholdPassed = encodedBytes >= hardProduct.Value();
    const bool cleanTargetPassed = encodedBytes >= cleanProduct.Value();
    REQUIRE(hardThresholdPassed);
    const double metric = static_cast<double>(encodedBytes) / static_cast<double>(uniqueFrames);
    const pbdemodd3d11::DemodSnapshot demodSnapshot = demodulator->GetSnapshot();
    REQUIRE(demodSnapshot.submittedFrames == uniqueFrames);
    REQUIRE(demodSnapshot.completedFrames == uniqueFrames);
    REQUIRE(demodSnapshot.pendingFrames == 0);
    REQUIRE(demodSnapshot.rawPixelReadbackBytes == 0);
    REQUIRE(environment.device->GetDeviceRemovedReason() == S_OK);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));

    const std::array<std::byte, 32> sourceDigest = pbprotocol::ComputeBlake3Digest(source);
    std::ostringstream line;
    line << std::setprecision(17)
         << "{\"schema\":\"PixelBridge.UnifiedPublishedPerformance.1\",\"name\":\"" << name << "\""
         << ",\"adapter\":" << AdapterJson(fingerprint)
         << ",\"sourceKind\":\"os-csprng\",\"compression\":\"RAW\",\"outerFec\":\"WirehairV2\""
         << ",\"sourceBlake3\":\"" << HexBytes(sourceDigest) << "\""
         << ",\"wholeFileBlake3\":\"" << HexBytes(manifest.wholeFileDigest.bytes) << "\""
         << ",\"chromaNeutralized\":" << (baseOnlyUniqueEquations ? "true" : "false")
         << ",\"scale\":" << (baseOnlyUniqueEquations ? "0.75" : "1.0")
         << ",\"initialControlSlots\":12,\"uniqueEquationPolicy\":\""
         << (baseOnlyUniqueEquations ? "base-luma-only-other-lanes-identical-duplicate" : "all-lanes") << "\""
         << ",\"systematicBlocks\":" << encoder.GetBlockCount()
         << ",\"uniqueBaseEquations\":" << uniqueBaseEquations
         << ",\"uniqueNonBaseEquations\":" << uniqueNonBaseEquations
         << ",\"metricNumeratorEncodedBytes\":" << encodedBytes
         << ",\"metricDenominatorUniqueLogicalFrames\":" << uniqueFrames
         << ",\"verifiedEncodedBytesPerUniqueFrame\":" << metric
         << ",\"hardThresholdBytesPerFrame\":" << hardThresholdBytes
         << ",\"hardThresholdPassed\":" << (hardThresholdPassed ? "true" : "false")
         << ",\"cleanEngineeringTargetBytesPerFrame\":" << cleanTargetBytes
         << ",\"cleanEngineeringTargetPassed\":" << (cleanTargetPassed ? "true" : "false")
         << ",\"falseAcceptedBlocks\":" << falseAcceptedBlocks
         << ",\"truthMismatchedBlocks\":" << truthMismatchedBlocks
         << ",\"conflictOutputBlocks\":0,\"wholeDigestVerified\":true,\"safePublish\":true"
         << ",\"finalReopenVerified\":true,\"metricAvailable\":true"
         << ",\"metricReadbackBytes\":" << demodSnapshot.metricReadbackBytes
         << ",\"rawPixelReadbackBytes\":" << demodSnapshot.rawPixelReadbackBytes << '}';
    WriteJsonLine(report, line.str());
    return {name, encodedBytes, uniqueFrames, metric, uniqueBaseEquations, uniqueNonBaseEquations,
        hardThresholdPassed, cleanTargetPassed};
}

} // namespace

TEST_CASE("G12 mandatory Unified corpus has final CPU GPU semantic parity on WARP and current vendor adapters",
    "[.unified-g12][g12][unified][gpu-parity][hardware]")
{
    std::ofstream report(PB_G12_PARITY_REPORT, std::ios::binary | std::ios::trunc);
    REQUIRE(report);
    const std::vector<PreparedMandatoryCase> preparedCases = PrepareMandatoryCases();
    D3DEnvironment warp = CreateWarpEnvironment();
    const AdapterFingerprint warpFingerprint = GetEnvironmentFingerprint(warp, "warp", true);
    const MandatoryParityRunSummary warpSummary = RunMandatoryParityCorpus(
        warp, warpFingerprint, preparedCases, report);
    RunMixedSemanticParity(warp, warpFingerprint, report);

    const std::vector<HardwareAdapterCandidate> candidates = EnumerateHardwareAdapters();
    std::uint32_t amdEnumerated = 0;
    std::uint32_t nvidiaEnumerated = 0;
    for (const HardwareAdapterCandidate& candidate : candidates)
    {
        amdEnumerated += static_cast<std::uint32_t>(candidate.fingerprint.description.VendorId == kAmdVendorId);
        nvidiaEnumerated += static_cast<std::uint32_t>(candidate.fingerprint.description.VendorId == kNvidiaVendorId);
    }
    std::uint32_t amdRuns = 0;
    std::uint32_t nvidiaRuns = 0;
    const auto RunVendor = [&](const std::uint32_t vendorId, std::uint32_t& runCount)
    {
        const auto selected = std::ranges::find_if(candidates,
            [vendorId](const HardwareAdapterCandidate& candidate)
            {
                return candidate.fingerprint.description.VendorId == vendorId &&
                    SUCCEEDED(candidate.deviceCreateStatus);
            });
        const bool vendorEnumerated = std::ranges::any_of(candidates,
            [vendorId](const HardwareAdapterCandidate& candidate)
            {
                return candidate.fingerprint.description.VendorId == vendorId;
            });
        if (!vendorEnumerated)
        {
            std::ostringstream line;
            line << "{\"schema\":\"PixelBridge.UnifiedGpuParity.AdapterAvailability.1\",\"vendorId\":"
                 << vendorId << ",\"enumerated\":false,\"run\":false}";
            WriteJsonLine(report, line.str());
            return;
        }
        REQUIRE(selected != candidates.end());
        D3DEnvironment environment;
        REQUIRE(SUCCEEDED(CreateHardwareEnvironment(selected->adapter.Get(), environment)));
        const MandatoryParityRunSummary summary = RunMandatoryParityCorpus(
            environment, selected->fingerprint, preparedCases, report);
        REQUIRE(summary.scenarioCount == unifiedtransformtest::kMandatoryTransformCaseCount);
        REQUIRE(summary.falseAcceptedBlocks == 0);
        REQUIRE(summary.truthMismatchedBlocks == 0);
        REQUIRE(summary.conflictOutputBlocks == 0);
        runCount++;
    };
    RunVendor(kAmdVendorId, amdRuns);
    RunVendor(kNvidiaVendorId, nvidiaRuns);
    REQUIRE(warpSummary.scenarioCount == unifiedtransformtest::kMandatoryTransformCaseCount);
    REQUIRE(warpSummary.falseAcceptedBlocks == 0);
    REQUIRE(warpSummary.truthMismatchedBlocks == 0);
    REQUIRE(warpSummary.conflictOutputBlocks == 0);
    REQUIRE(amdRuns <= 1);
    REQUIRE(nvidiaRuns <= 1);
    std::ostringstream summaryLine;
    summaryLine << "{\"schema\":\"PixelBridge.UnifiedGpuParity.Summary.1\",\"mandatoryScenarios\":"
                << preparedCases.size() << ",\"warpRuns\":1,\"amdEnumerated\":" << amdEnumerated
                << ",\"amdRuns\":" << amdRuns << ",\"nvidiaEnumerated\":" << nvidiaEnumerated
                << ",\"nvidiaRuns\":" << nvidiaRuns
                << ",\"semanticMutationScenarios\":2,\"falseAcceptedBlocks\":0"
                << ",\"truthMismatchedBlocks\":0,\"conflictOutputBlocks\":0}";
    WriteJsonLine(report, summaryLine.str());
    report.flush();
    REQUIRE(report);
    std::cout << "PB_G12_UNIFIED_PARITY_JSON=" << summaryLine.str() << '\n';
}

TEST_CASE("G12 CSPRNG RAW Unified WARP recovery publishes before reporting verified bytes per unique frame",
    "[.unified-g12][g12][unified][warp][receiver][storage][performance]")
{
    std::ofstream report(PB_G12_PERFORMANCE_REPORT, std::ios::binary | std::ios::trunc);
    REQUIRE(report);
    D3DEnvironment warp = CreateWarpEnvironment();
    const AdapterFingerprint fingerprint = GetEnvironmentFingerprint(warp, "warp", true);
    const PerformanceResult baseResult = RunPublishedPerformanceCase(warp, fingerprint, true, report);
    const PerformanceResult cleanResult = RunPublishedPerformanceCase(warp, fingerprint, false, report);
    REQUIRE(baseResult.hardThresholdPassed);
    REQUIRE(baseResult.uniqueNonBaseEquations == 0);
    REQUIRE(cleanResult.hardThresholdPassed);
    std::ostringstream summaryLine;
    summaryLine << std::setprecision(17)
                << "{\"schema\":\"PixelBridge.UnifiedPublishedPerformance.Summary.1\""
                << ",\"baseVerifiedEncodedBytesPerUniqueFrame\":"
                << baseResult.verifiedEncodedBytesPerUniqueFrame
                << ",\"baseHardThresholdPassed\":" << (baseResult.hardThresholdPassed ? "true" : "false")
                << ",\"cleanVerifiedEncodedBytesPerUniqueFrame\":"
                << cleanResult.verifiedEncodedBytesPerUniqueFrame
                << ",\"cleanEngineeringTargetPassed\":" << (cleanResult.cleanTargetPassed ? "true" : "false")
                << ",\"metricsAvailableOnlyAfterPublishAndReopen\":true}";
    WriteJsonLine(report, summaryLine.str());
    report.flush();
    REQUIRE(report);
    std::cout << "PB_G12_UNIFIED_PERFORMANCE_JSON=" << summaryLine.str() << '\n';
}
