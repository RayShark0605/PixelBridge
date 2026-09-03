#include "pbdemodd3d11/demodulator.h"

#include "demod_shader_source.h"
#include "remote_visual_low_fps_shader_source.h"
#include "unified_visual_shader_source.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbmodulation/unified_visual.h"
#include "pbinterleave/tile_permutation.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <utility>

namespace pbdemodd3d11
{
namespace
{

using Microsoft::WRL::ComPtr;
using pbcapturenormalize::CaptureSignalEncoding;
using pbcapturenormalize::ScreenCaptureDomain;
using pbcapturenormalize::ScreenCaptureFrame;

inline constexpr std::uint32_t minimumSlots = 2;
inline constexpr std::uint32_t maximumSlots = 4;
inline constexpr std::uint32_t maximumMetricCount = static_cast<std::uint32_t>(pbmodulation::kDesktopLevelsMaximumBits);
inline constexpr std::uint32_t maximumMetricBytes = maximumMetricCount * sizeof(float);
inline constexpr std::uint32_t calibrationPilotCount = 4;
inline constexpr std::uint32_t calibrationStateCount = 4;
inline constexpr std::uint32_t legacyCalibrationEntries = calibrationPilotCount * calibrationStateCount;
inline constexpr std::uint32_t unifiedCalibrationEntries = 36;
inline constexpr std::uint32_t calibrationEntries = unifiedCalibrationEntries;
inline constexpr std::uint32_t legacyCalibrationBytes = legacyCalibrationEntries * sizeof(float) * 4;
inline constexpr std::uint32_t calibrationBytes = calibrationEntries * sizeof(float) * 4;
inline constexpr std::uint32_t remoteVisualLowFpsFreshnessSummaryEntries =
    pbmodulation::kRemoteVisualFreshnessRegionCount + 1;
inline constexpr std::uint32_t remoteVisualLowFpsFreshnessSummaryBytes =
    remoteVisualLowFpsFreshnessSummaryEntries * sizeof(std::uint32_t) * 4;
inline constexpr std::uint32_t remoteVisualLowFpsTileMappingBytes =
    pbmodulation::kRemoteVisualTileCount * sizeof(std::uint32_t) * 4;
inline constexpr std::uint32_t remoteVisualLowFpsFrameBindingBytes = remoteVisualLowFpsTileMappingBytes;
inline constexpr std::uint32_t remoteVisualLowFpsSymbolMaskBytes =
    static_cast<std::uint32_t>(pbmodulation::kRemoteVisualLowFpsSymbolMasks.size() * sizeof(std::uint32_t));
inline constexpr std::uint32_t remoteVisualLowFpsSamplesPerTile = 16 * 5;
inline constexpr std::uint32_t remoteVisualLowFpsCalibrationSamples = 4 * 2 * 8 * 8;
inline constexpr std::uint32_t remoteVisualLowFpsMaximumTexelsPerSample = 4;
inline constexpr std::uint32_t unifiedTileBindingBytes =
    pbmodulation::kUnifiedVisualProfile.dataTileCount * sizeof(std::uint32_t) * 8;
inline constexpr std::uint32_t unifiedExpectedFreshnessBytes =
    pbmodulation::kUnifiedFreshnessRegionCount * pbmodulation::kLocalDesktopTimingBits * sizeof(std::uint32_t);
inline constexpr std::uint32_t unifiedTileSamplingBytes =
    pbmodulation::kUnifiedVisualProfile.dataTileCount * sizeof(std::uint32_t);
inline constexpr std::uint32_t unifiedFreshnessBytes =
    pbmodulation::kUnifiedFreshnessRegionCount * sizeof(float) * 4;
inline constexpr std::uint32_t unifiedPhaseEntries = 16;
inline constexpr std::uint32_t unifiedPhaseBytes = unifiedPhaseEntries * sizeof(float) * 4;
inline constexpr std::uint32_t unifiedSymbolMaskBytes =
    static_cast<std::uint32_t>(pbmodulation::kUnifiedSymbolMasksByLabel.size() * sizeof(std::uint32_t));
static_assert(pbmodulation::kRemoteVisualLowFpsBitsPerTile == 4);
static_assert(pbmodulation::kRemoteVisualLowFpsSymbolMasks.size() == 16);
static_assert(pbmodulation::kRemoteVisualLowFpsCodedBits == 64800);
static_assert(maximumMetricCount >= pbmodulation::kUnifiedSoftMetricCount);
static_assert(pbmodulation::kUnifiedCodedFrameBytes <= pbmodulation::kDesktopLevelsMaximumDataBytes);
static_assert(static_cast<std::uint32_t>(pbmodulation::RemoteVisualTileRole::FreshnessTag) == 1);
static_assert(static_cast<std::uint32_t>(pbmodulation::RemoteVisualTileRole::Data) == 2);
static_assert(pbmodulation::kRemoteVisualLadders[0].x == 736 && pbmodulation::kRemoteVisualLadders[0].y == 16);
static_assert(pbmodulation::kRemoteVisualLadders[1].x == 1696 && pbmodulation::kRemoteVisualLadders[1].y == 16);
static_assert(pbmodulation::kRemoteVisualLadders[2].x == 96 && pbmodulation::kRemoteVisualLadders[2].y == 1000);
static_assert(pbmodulation::kRemoteVisualLadders[3].x == 1056 && pbmodulation::kRemoteVisualLadders[3].y == 1000);
static_assert(std::ranges::all_of(pbmodulation::kRemoteVisualLadders,
    [](const pbmodulation::LocalDesktopRegion& region) { return region.width == 128 && region.height == 64; }));
static_assert(pbmodulation::kUnifiedVisualProfile.tileWidth == 4 && pbmodulation::kUnifiedVisualProfile.tileHeight == 4);
static_assert(pbmodulation::kUnifiedVisualProfile.dataTileCount == 86688);
static_assert(pbmodulation::kUnifiedSoftMetricCount == 502200);
static_assert(pbmodulation::kUnifiedFreshnessRegionCount == 9 && pbmodulation::kLocalDesktopTimingBits == 256);
static_assert(pbmodulation::kUnifiedSymbolMasksByLabel.size() == 16);
static_assert(pbmodulation::kUnifiedCalibrationLumaRows == 24 && pbmodulation::kUnifiedCalibrationNeutralRows == 8 &&
    pbmodulation::kUnifiedCalibrationChromaRows == 32);
static_assert(pbmodulation::kUnifiedVisualProfile.regions[6].bounds == pbmodulation::UnifiedPixelRegion{96, 160, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[7].bounds == pbmodulation::UnifiedPixelRegion{896, 160, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[8].bounds == pbmodulation::UnifiedPixelRegion{1696, 160, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[9].bounds == pbmodulation::UnifiedPixelRegion{96, 476, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[10].bounds == pbmodulation::UnifiedPixelRegion{896, 476, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[11].bounds == pbmodulation::UnifiedPixelRegion{1696, 476, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[12].bounds == pbmodulation::UnifiedPixelRegion{96, 792, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[13].bounds == pbmodulation::UnifiedPixelRegion{896, 792, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[14].bounds == pbmodulation::UnifiedPixelRegion{1696, 792, 128, 128});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[15].bounds == pbmodulation::UnifiedPixelRegion{736, 16, 128, 64});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[16].bounds == pbmodulation::UnifiedPixelRegion{1696, 16, 128, 64});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[17].bounds == pbmodulation::UnifiedPixelRegion{96, 1000, 128, 64});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[18].bounds == pbmodulation::UnifiedPixelRegion{1056, 1000, 128, 64});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[19].bounds == pbmodulation::UnifiedPixelRegion{896, 16, 128, 64});
static_assert(pbmodulation::kUnifiedVisualProfile.regions[20].bounds == pbmodulation::UnifiedPixelRegion{896, 1000, 128, 64});

enum class ProfileMode : std::uint32_t
{
    DesktopLevels2 = 1, DesktopLevels4 = 2, ShapeChroma = 3, RemoteVisual = 4, RemoteVisualLowFps = 5,
    UnifiedVisual = 6
};

struct Binding
{
    ProfileMode mode = ProfileMode::ShapeChroma;
    std::uint64_t profileId = 0;
    std::uint32_t tilePixels = 0;
    std::uint32_t tileCount = 0;
    std::uint32_t rowTiles = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t metricCount = 0;
    std::uint32_t codedMetricCount = 0;
    std::uint32_t codewords = 0;
    std::uint32_t paddingBytes = 0;
    std::uint32_t interleavePhase = 0;
    std::uint64_t sessionTag = 0;
    std::uint64_t frameSequence = 0;
};

struct alignas(16) FrameConstants
{
    std::uint32_t mode;
    std::uint32_t tilePixels;
    std::uint32_t tileCount;
    std::uint32_t rowTiles;
    std::uint32_t interleavePhase;
    std::uint32_t metricCount;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    float originX;
    float originY;
    float scaleX;
    float scaleY;
    float sourceTexelPitchX;
    float sourceTexelPitchY;
    float minimumEndpointSeparation;
    float maximumPilotVariance;
    float maximumPilotSpatialDeviation;
    float minimumSymbolRms;
    float maximumSymbolResidual;
    float minimumSymbolMargin;
    float minimumFreshnessMetric;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    std::uint32_t freshnessRegionCount;
    float bootstrapBlackLevel;
    float bootstrapWhiteLevel;
    float reservedFloat0;
    float reservedFloat1;
};
static_assert(sizeof(FrameConstants) == 112);

struct alignas(16) RemoteVisualLowFpsTileMapping
{
    std::uint32_t originX = 0;
    std::uint32_t originY = 0;
    std::uint32_t role = 0;
    std::uint32_t regionId = 0;
};
static_assert(sizeof(RemoteVisualLowFpsTileMapping) == 16);

struct alignas(16) RemoteVisualLowFpsFrameBinding
{
    std::array<std::uint32_t, pbmodulation::kRemoteVisualLowFpsBitsPerTile> logicalBits{};
};
static_assert(sizeof(RemoteVisualLowFpsFrameBinding) == 16);

struct alignas(16) RemoteVisualLowFpsFreshnessSummary
{
    std::uint32_t mismatches = 0;
    std::uint32_t erasures = 0;
    std::uint32_t stale = 0;
    std::uint32_t erasedDataMetrics = 0;
};
static_assert(sizeof(RemoteVisualLowFpsFreshnessSummary) == 16);

struct alignas(16) UnifiedTileBinding
{
    std::uint32_t originX = 0;
    std::uint32_t originY = 0;
    std::array<std::uint32_t, 4> lumaBits{};
    std::array<std::uint32_t, 2> chromaBits{};
};
static_assert(sizeof(UnifiedTileBinding) == 32);

bool EqualLuid(const LUID& left, const LUID& right) noexcept
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

bool NonzeroSourceId(const ScreenCaptureDomain& domain) noexcept
{
    return std::ranges::any_of(domain.sourceId, [](const std::byte value) { return value != std::byte{0}; });
}

bool SupportedSourceFormat(const DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool SameComIdentity(IUnknown* left, IUnknown* right) noexcept
{
    if (left == nullptr || right == nullptr)
    {
        return false;
    }
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) &&
        leftIdentity.Get() == rightIdentity.Get();
}

DemodStatus GetAdapterLuid(ID3D11Device* device, LUID& output) noexcept
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(result))
    {
        result = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result))
    {
        result = adapter->GetDesc(&description);
    }
    if (FAILED(result))
    {
        return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Device, result);
    }
    output = description.AdapterLuid;
    return {};
}

DemodStatus ParseBinding(const std::span<const std::byte> bytes, Binding& output) noexcept
{
    if (bytes.size() != pbprotocol::kBootstrapRecordBytes)
    {
        return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding,
            static_cast<std::int32_t>(pbprotocol::ProtocolErrorCode::InvalidRecordSize));
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bytes);
    if (!parsed)
    {
        return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding,
            static_cast<std::int32_t>(parsed.Error().code));
    }
    Binding binding;
    binding.profileId = parsed.Value().visualProfileId;
    binding.interleavePhase = static_cast<std::uint32_t>(parsed.Value().frameSequence % 16);
    binding.sessionTag = parsed.Value().sessionTag.value;
    binding.frameSequence = parsed.Value().frameSequence;
    if (binding.profileId == pbmodulation::kShapeChromaProfileId &&
        parsed.Value().visualLayoutVersion == pbmodulation::kShapeChromaLayoutVersion)
    {
        binding.mode = ProfileMode::ShapeChroma;
        binding.tilePixels = pbmodulation::kShapeChromaTilePixels;
        binding.tileCount = pbmodulation::kShapeChromaTileCount;
        binding.rowTiles = 432;
        binding.dataBytes = pbmodulation::kShapeChromaDataBytes;
        binding.metricCount = static_cast<std::uint32_t>(pbmodulation::kShapeChromaMaximumBits);
        binding.codedMetricCount = binding.metricCount;
        binding.codewords = pbmodulation::kShapeChromaCodewords;
        binding.paddingBytes = pbmodulation::kShapeChromaPaddingBytes;
    }
    else if (binding.profileId == pbmodulation::kRemoteVisualProfileId &&
        parsed.Value().visualLayoutVersion == pbmodulation::kRemoteVisualLayoutVersion)
    {
        binding.mode = ProfileMode::RemoteVisual;
        binding.tilePixels = pbmodulation::kRemoteVisualTilePixels;
        binding.tileCount = pbmodulation::kRemoteVisualTileCount;
        binding.rowTiles = 0;
        binding.dataBytes = pbmodulation::kRemoteVisualDataBytes;
        binding.metricCount = pbmodulation::kRemoteVisualTileCount;
        binding.codedMetricCount = pbmodulation::kRemoteVisualCodedBits;
        binding.codewords = pbmodulation::kRemoteVisualCodewords;
        binding.paddingBytes = pbmodulation::kRemoteVisualPaddingBytes;
    }
    else if (binding.profileId == pbmodulation::kRemoteVisualLowFpsProfileId &&
        parsed.Value().visualLayoutVersion == pbmodulation::kRemoteVisualLowFpsLayoutVersion)
    {
        binding.mode = ProfileMode::RemoteVisualLowFps;
        binding.tilePixels = pbmodulation::kRemoteVisualTilePixels;
        binding.tileCount = pbmodulation::kRemoteVisualTileCount;
        binding.rowTiles = 0;
        binding.dataBytes = pbmodulation::kRemoteVisualLowFpsDataBytes;
        binding.metricCount = pbmodulation::kRemoteVisualLowFpsCodedBits;
        binding.codedMetricCount = pbmodulation::kRemoteVisualLowFpsCodedBits;
        binding.codewords = pbmodulation::kRemoteVisualLowFpsCodewords;
        binding.paddingBytes = pbmodulation::kRemoteVisualLowFpsPaddingBytes;
    }
    else if (binding.profileId == pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId &&
        parsed.Value().visualLayoutVersion == pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion)
    {
        binding.mode = ProfileMode::UnifiedVisual;
        binding.tilePixels = pbmodulation::kUnifiedVisualProfile.tileWidth;
        binding.tileCount = pbmodulation::kUnifiedVisualProfile.dataTileCount;
        binding.rowTiles = 0;
        binding.dataBytes = static_cast<std::uint32_t>(pbmodulation::kUnifiedCodedFrameBytes);
        binding.metricCount = static_cast<std::uint32_t>(pbmodulation::kUnifiedSoftMetricCount);
        binding.codedMetricCount = binding.metricCount;
        binding.codewords = pbmodulation::kUnifiedCodewordCount;
        binding.paddingBytes = 0;
    }
    else
    {
        const auto* const profile = pbmodulation::GetDesktopLevelsProfile(binding.profileId);
        if (profile == nullptr || parsed.Value().visualLayoutVersion != pbmodulation::kDesktopLevelsLayoutVersion)
        {
            return DemodStatus::Failure(DemodError::UnsupportedProfile, DemodStage::Binding);
        }
        binding.mode = profile->tilePixels == 2 ? ProfileMode::DesktopLevels2 : ProfileMode::DesktopLevels4;
        binding.tilePixels = profile->tilePixels;
        binding.tileCount = profile->tileCount;
        binding.rowTiles = 1728 / profile->tilePixels;
        binding.dataBytes = profile->dataBytes;
        binding.metricCount = profile->dataBytes * 8;
        binding.codedMetricCount = binding.metricCount;
        binding.codewords = profile->codewords;
        binding.paddingBytes = profile->paddingBytes;
    }
    if (binding.metricCount > maximumMetricCount || binding.codedMetricCount == 0 ||
        binding.codedMetricCount > binding.metricCount || binding.codedMetricCount != binding.dataBytes * 8 ||
        binding.codewords > pbdesktoplevels::kMaximumCodewords ||
        binding.codewords * pbdesktoplevels::kCodewordBytes + binding.paddingBytes != binding.dataBytes)
    {
        return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
    }
    output = binding;
    return {};
}

DemodStatus CompileShader(ID3D11Device* device, const char* source, const std::size_t sourceBytes,
    const char* entryPoint, ComPtr<ID3D11ComputeShader>& output) noexcept
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT compile = D3DCompile(source, sourceBytes,
        "PB-Demod-D3D11", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &shader, &diagnostics);
    if (FAILED(compile))
    {
        return DemodStatus::Failure(DemodError::ShaderCompileFailure, DemodStage::Shader, compile);
    }
    const HRESULT create = device->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &output);
    return FAILED(create) ? DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Shader, create) : DemodStatus{};
}

DemodStatus CreateStructuredBuffer(ID3D11Device* device, const std::uint32_t bytes, const std::uint32_t stride,
    const std::uint32_t bindFlags, const D3D11_USAGE usage, const std::uint32_t cpuAccess, ComPtr<ID3D11Buffer>& output) noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = usage;
    description.BindFlags = bindFlags;
    description.CPUAccessFlags = cpuAccess;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    const HRESULT result = device->CreateBuffer(&description, nullptr, &output);
    return FAILED(result) ? DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, result) : DemodStatus{};
}

DemodStatus CreateImmutableStructuredBuffer(ID3D11Device* device, const std::uint32_t bytes, const std::uint32_t stride,
    const void* const data, ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11ShaderResourceView>& view) noexcept
{
    if (data == nullptr || bytes == 0 || stride == 0 || bytes % stride != 0)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Resource);
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    HRESULT result = device->CreateBuffer(&description, &initial, &buffer);
    if (SUCCEEDED(result))
    {
        result = device->CreateShaderResourceView(buffer.Get(), nullptr, &view);
    }
    return FAILED(result) ? DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, result) : DemodStatus{};
}

DemodStatus CreateConstantBuffer(ID3D11Device* device, ComPtr<ID3D11Buffer>& output) noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(FrameConstants);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const HRESULT result = device->CreateBuffer(&description, nullptr, &output);
    return FAILED(result) ? DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, result) : DemodStatus{};
}

bool FiniteCalibration(const std::array<std::array<float, 4>, calibrationEntries>& values) noexcept
{
    return std::ranges::all_of(values, [](const auto& entry)
    {
        return std::ranges::all_of(entry, [](const float value) { return std::isfinite(value); });
    });
}

DemodStatus ValidateCalibration(const ProfileMode mode,
    const std::array<std::array<float, 4>, calibrationEntries>& calibration,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy* const remoteVisualLowFpsPolicy = nullptr) noexcept
{
    if (!FiniteCalibration(calibration))
    {
        return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
    }
    if (mode == ProfileMode::ShapeChroma)
    {
        std::array<std::array<double, 2>, calibrationStateCount> centroids{};
        for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
        {
            for (std::size_t state = 0; state < calibrationStateCount; state++)
            {
                const auto& entry = calibration[pilot * calibrationStateCount + state];
                if (entry[2] > 64 || entry[3] != 0)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
                centroids[state][0] += entry[0] / calibrationPilotCount;
                centroids[state][1] += entry[1] / calibrationPilotCount;
            }
        }
        double minimumSeparation = std::numeric_limits<double>::max();
        for (std::size_t state = 0; state < calibrationStateCount; state++)
        {
            for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
            {
                const auto& entry = calibration[pilot * calibrationStateCount + state];
                const double blue = static_cast<double>(entry[0]) - centroids[state][0];
                const double red = static_cast<double>(entry[1]) - centroids[state][1];
                if (std::sqrt(blue * blue + red * red) > 8)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
            }
            for (std::size_t other = 0; other < state; other++)
            {
                const double blue = centroids[state][0] - centroids[other][0];
                const double red = centroids[state][1] - centroids[other][1];
                minimumSeparation = std::min(minimumSeparation, std::sqrt(blue * blue + red * red));
            }
        }
        return minimumSeparation < 16 ? DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration) : DemodStatus{};
    }
    if (mode == ProfileMode::RemoteVisual || mode == ProfileMode::RemoteVisualLowFps)
    {
        if (mode == ProfileMode::RemoteVisualLowFps && remoteVisualLowFpsPolicy == nullptr)
        {
            return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
        }
        const double maximumVariance = mode == ProfileMode::RemoteVisualLowFps ?
            remoteVisualLowFpsPolicy->maximumPilotStandardDeviation * remoteVisualLowFpsPolicy->maximumPilotStandardDeviation : 576;
        const double minimumSeparation = mode == ProfileMode::RemoteVisualLowFps ?
            remoteVisualLowFpsPolicy->minimumEndpointSeparation : 96;
        const double maximumSpatialDeviation = mode == ProfileMode::RemoteVisualLowFps ?
            remoteVisualLowFpsPolicy->maximumPilotSpatialDeviation : 24;
        std::array<double, 2> centroids{};
        for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
        {
            for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
            {
                const std::size_t level = endpoint == 0 ? 0 : 3;
                const auto& entry = calibration[pilot * calibrationStateCount + level];
                if (entry[1] > maximumVariance || entry[2] != 0 || entry[3] != 0)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
                centroids[endpoint] += entry[0] / calibrationPilotCount;
            }
            if (mode == ProfileMode::RemoteVisualLowFps &&
                calibration[pilot * calibrationStateCount + 3][0] - calibration[pilot * calibrationStateCount][0] < minimumSeparation)
            {
                return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
            }
        }
        for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
        {
            const std::size_t level = endpoint == 0 ? 0 : 3;
            if (mode == ProfileMode::RemoteVisualLowFps)
            {
                double minimumCentroid = calibration[level][0];
                double maximumCentroid = minimumCentroid;
                for (std::size_t pilot = 1; pilot < calibrationPilotCount; pilot++)
                {
                    const double centroid = calibration[pilot * calibrationStateCount + level][0];
                    minimumCentroid = std::min(minimumCentroid, centroid);
                    maximumCentroid = std::max(maximumCentroid, centroid);
                }
                if (maximumCentroid - minimumCentroid > maximumSpatialDeviation)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
            }
            else
            {
                for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
                {
                    if (std::abs(static_cast<double>(calibration[pilot * calibrationStateCount + level][0]) - centroids[endpoint]) >
                        maximumSpatialDeviation)
                    {
                        return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                    }
                }
            }
        }
        return centroids[1] - centroids[0] < minimumSeparation ?
            DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration) : DemodStatus{};
    }
    std::array<double, calibrationStateCount> centroids{};
    for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
    {
        for (std::size_t level = 0; level < calibrationStateCount; level++)
        {
            const auto& entry = calibration[pilot * calibrationStateCount + level];
            if (entry[1] > 64 || entry[2] != 0 || entry[3] != 0)
            {
                return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
            }
            centroids[level] += entry[0] / calibrationPilotCount;
        }
    }
    for (std::size_t level = 0; level < calibrationStateCount; level++)
    {
        for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
        {
            if (std::abs(static_cast<double>(calibration[pilot * calibrationStateCount + level][0]) - centroids[level]) > 8)
            {
                return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
            }
        }
        if (level != 0 && centroids[level] - centroids[level - 1] < 32)
        {
            return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
        }
    }
    return {};
}

DemodStatus ResolveUnifiedCalibration(const std::array<std::array<float, 4>, calibrationEntries>& calibration,
    const pbmodulation::UnifiedVisualDecodePolicy& policy,
    pbmodulation::UnifiedBaseLumaObservation& baseLuma,
    pbmodulation::UnifiedFineLumaObservation& fineLuma,
    pbmodulation::UnifiedChromaObservation& chroma) noexcept
{
    if (!FiniteCalibration(calibration))
    {
        return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
    }
    const double maximumVariance = policy.maximumPilotDeviation * policy.maximumPilotDeviation;
    std::array<double, 4> lumaCentroids{};
    std::array<std::array<double, 2>, 4> chromaCentroids{};
    bool lumaValid = true;
    bool chromaValid = true;
    for (std::size_t label = 0; label < 4; label++)
    {
        double lumaSecondMoment = 0;
        std::array<double, 2> chromaSecondMoment{};
        for (std::size_t pilot = 0; pilot < 4; pilot++)
        {
            const auto& lumaEntry = calibration[pilot * 4 + label];
            const auto& chromaEntry = calibration[16 + pilot * 4 + label];
            lumaCentroids[label] += static_cast<double>(lumaEntry[0]) / 4;
            lumaSecondMoment += (static_cast<double>(lumaEntry[1]) +
                static_cast<double>(lumaEntry[0]) * lumaEntry[0]) / 4;
            chromaCentroids[label][0] += static_cast<double>(chromaEntry[0]) / 4;
            chromaCentroids[label][1] += static_cast<double>(chromaEntry[1]) / 4;
            chromaSecondMoment[0] += (static_cast<double>(chromaEntry[2]) +
                static_cast<double>(chromaEntry[0]) * chromaEntry[0]) / 4;
            chromaSecondMoment[1] += (static_cast<double>(chromaEntry[3]) +
                static_cast<double>(chromaEntry[1]) * chromaEntry[1]) / 4;
            lumaValid = lumaValid && lumaEntry[2] == 1.0f && lumaEntry[1] >= 0;
            chromaValid = chromaValid && chromaEntry[2] >= 0 && chromaEntry[3] >= 0;
        }
        lumaValid = lumaValid && lumaSecondMoment - lumaCentroids[label] * lumaCentroids[label] <= maximumVariance;
        chromaValid = chromaValid &&
            chromaSecondMoment[0] - chromaCentroids[label][0] * chromaCentroids[label][0] <= maximumVariance &&
            chromaSecondMoment[1] - chromaCentroids[label][1] * chromaCentroids[label][1] <= maximumVariance;
        for (std::size_t pilot = 0; pilot < 4; pilot++)
        {
            const auto& lumaEntry = calibration[pilot * 4 + label];
            const auto& chromaEntry = calibration[16 + pilot * 4 + label];
            lumaValid = lumaValid && std::abs(static_cast<double>(lumaEntry[0]) - lumaCentroids[label]) <=
                policy.maximumPilotDeviation;
            chromaValid = chromaValid && std::hypot(
                static_cast<double>(chromaEntry[0]) - chromaCentroids[label][0],
                static_cast<double>(chromaEntry[1]) - chromaCentroids[label][1]) <= policy.maximumPilotDeviation;
        }
        if (label != 0)
        {
            lumaValid = lumaValid && lumaCentroids[label] - lumaCentroids[label - 1] >= policy.minimumLumaLevelGap;
        }
    }
    std::array<double, 2> neutralCentroid{};
    for (std::size_t pilot = 0; pilot < 4; pilot++)
    {
        const auto& neutralEntry = calibration[32 + pilot];
        neutralCentroid[0] += static_cast<double>(neutralEntry[0]) / 4;
        neutralCentroid[1] += static_cast<double>(neutralEntry[1]) / 4;
        chromaValid = chromaValid && neutralEntry[2] == 1.0f;
    }
    chromaValid = chromaValid && std::hypot(neutralCentroid[0], neutralCentroid[1]) <=
        policy.maximumPilotDeviation;
    for (std::size_t label = 0; label < 4; label++)
    {
        for (std::size_t other = 0; other < label; other++)
        {
            chromaValid = chromaValid && std::hypot(
                chromaCentroids[label][0] - chromaCentroids[other][0],
                chromaCentroids[label][1] - chromaCentroids[other][1]) >= policy.minimumChromaSeparation;
        }
    }
    if (!lumaValid)
    {
        baseLuma = {pbmodulation::UnifiedErasureScope::Lane,
            pbmodulation::UnifiedErasureReason::BaseLumaPilotFailure};
        fineLuma = {pbmodulation::UnifiedErasureScope::Lane,
            pbmodulation::UnifiedErasureReason::FineLumaPilotFailure};
    }
    if (!chromaValid)
    {
        chroma = {pbmodulation::UnifiedErasureScope::Lane,
            pbmodulation::UnifiedErasureReason::ChromaPilotFailure};
    }
    return {};
}

DemodStatus ApplyUnifiedPhaseObservations(
    const std::array<std::array<float, 4>, unifiedPhaseEntries>& phases,
    const std::uint64_t frameSequence, const pbmodulation::UnifiedVisualDecodePolicy& policy,
    const std::array<std::array<float, 4>, calibrationEntries>& calibration,
    pbmodulation::UnifiedBaseLumaObservation& baseLuma,
    pbmodulation::UnifiedFineLumaObservation& fineLuma) noexcept
{
    if (!std::ranges::all_of(phases, [](const auto& entry)
        {
            return std::ranges::all_of(entry, [](const float value) { return std::isfinite(value); });
        }))
    {
        return DemodStatus::Failure(DemodError::NonFiniteMetric, DemodStage::Readback);
    }
    double low = 0;
    double high = 0;
    for (std::size_t pilot = 0; pilot < 4; pilot++)
    {
        low += static_cast<double>(calibration[pilot * 4 + 1][0]) / 4;
        high += static_cast<double>(calibration[pilot * 4 + 2][0]) / 4;
    }
    const double gap = high - low;
    const std::size_t expectedPhase = static_cast<std::size_t>(frameSequence % 8);
    for (std::size_t phasePilot = 0; phasePilot < 2; phasePilot++)
    {
        double nearestOther = (std::numeric_limits<double>::max)();
        bool entriesValid = gap > 0;
        for (std::size_t phase = 0; phase < 8; phase++)
        {
            const auto& entry = phases[phasePilot * 8 + phase];
            entriesValid = entriesValid && entry[0] >= 0 && entry[1] == 512.0f && entry[2] == 1.0f;
            if (phase != expectedPhase)
            {
                nearestOther = std::min(nearestOther, static_cast<double>(entry[0]));
            }
        }
        const double expectedDistance = phases[phasePilot * 8 + expectedPhase][0];
        const double normalizedResidual = gap > 0 ? expectedDistance / (512.0 * 16.0 * gap * gap) :
            (std::numeric_limits<double>::infinity)();
        const bool valid = entriesValid && expectedDistance < nearestOther &&
            normalizedResidual <= policy.maximumPhasePilotResidual;
        if (!valid && phasePilot == 0)
        {
            baseLuma = {pbmodulation::UnifiedErasureScope::Lane,
                pbmodulation::UnifiedErasureReason::BaseLumaPilotFailure};
        }
        else if (!valid)
        {
            fineLuma = {pbmodulation::UnifiedErasureScope::Lane,
                pbmodulation::UnifiedErasureReason::FineLumaPilotFailure};
        }
    }
    return {};
}

} // namespace

struct Demodulator::Implementation
{
    struct Slot
    {
        ComPtr<ID3D11Buffer> metrics;
        ComPtr<ID3D11Buffer> metricsStaging;
        ComPtr<ID3D11UnorderedAccessView> metricsUav;
        ComPtr<ID3D11Buffer> calibration;
        ComPtr<ID3D11Buffer> calibrationStaging;
        ComPtr<ID3D11UnorderedAccessView> calibrationUav;
        ComPtr<ID3D11ShaderResourceView> calibrationSrv;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11Buffer> remoteVisualLowFpsFrameBindings;
        ComPtr<ID3D11ShaderResourceView> remoteVisualLowFpsFrameBindingsSrv;
        ComPtr<ID3D11Buffer> remoteVisualLowFpsFreshnessSummary;
        ComPtr<ID3D11Buffer> remoteVisualLowFpsFreshnessSummaryStaging;
        ComPtr<ID3D11UnorderedAccessView> remoteVisualLowFpsFreshnessSummaryUav;
        ComPtr<ID3D11Buffer> unifiedTileBindings;
        ComPtr<ID3D11ShaderResourceView> unifiedTileBindingsSrv;
        ComPtr<ID3D11Buffer> unifiedExpectedFreshness;
        ComPtr<ID3D11ShaderResourceView> unifiedExpectedFreshnessSrv;
        ComPtr<ID3D11Buffer> unifiedTileSamplingFailures;
        ComPtr<ID3D11Buffer> unifiedTileSamplingFailuresStaging;
        ComPtr<ID3D11UnorderedAccessView> unifiedTileSamplingFailuresUav;
        ComPtr<ID3D11Buffer> unifiedFreshness;
        ComPtr<ID3D11Buffer> unifiedFreshnessStaging;
        ComPtr<ID3D11UnorderedAccessView> unifiedFreshnessUav;
        ComPtr<ID3D11Buffer> unifiedPhase;
        ComPtr<ID3D11Buffer> unifiedPhaseStaging;
        ComPtr<ID3D11UnorderedAccessView> unifiedPhaseUav;
        ComPtr<ID3D11Query> completion;
        ComPtr<ID3D11Query> timestampDisjoint;
        ComPtr<ID3D11Query> timestampStart;
        ComPtr<ID3D11Query> timestampEnd;
        ComPtr<ID3D11Texture2D> inputTexture;
        ComPtr<ID3D11ShaderResourceView> inputSrv;
        ScreenCaptureFrame frame;
        Binding binding;
        pbmodulation::LocalDesktopGeometry remoteVisualLowFpsGeometry;
        pbmodulation::RemoteVisualLowFpsDecodePolicy remoteVisualLowFpsPolicy;
        pbmodulation::LocalDesktopObservation unifiedBootstrap;
        pbmodulation::UnifiedVisualDecodePolicy unifiedPolicy;
        std::array<std::byte, 44> bootstrapRecord{};
        std::uint64_t generation = 0;
        bool busy = false;
        bool cancelled = false;
        bool unbound = false;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> calibrateChroma;
    ComPtr<ID3D11ComputeShader> calibrateLevels;
    ComPtr<ID3D11ComputeShader> demodShapeChroma;
    ComPtr<ID3D11ComputeShader> demodDesktopLevels;
    ComPtr<ID3D11ComputeShader> demodRemoteVisual;
    ComPtr<ID3D11ComputeShader> calibrateRemoteVisualLowFps;
    ComPtr<ID3D11ComputeShader> demodRemoteVisualLowFpsFreshness;
    ComPtr<ID3D11ComputeShader> demodRemoteVisualLowFps;
    ComPtr<ID3D11ComputeShader> calibrateUnified;
    ComPtr<ID3D11ComputeShader> evaluateUnifiedFreshness;
    ComPtr<ID3D11ComputeShader> evaluateUnifiedPhase;
    ComPtr<ID3D11ComputeShader> demodUnified;
    ComPtr<ID3D11Buffer> remoteVisualLowFpsTileMappings;
    ComPtr<ID3D11ShaderResourceView> remoteVisualLowFpsTileMappingsSrv;
    ComPtr<ID3D11Buffer> remoteVisualLowFpsSymbolMasks;
    ComPtr<ID3D11ShaderResourceView> remoteVisualLowFpsSymbolMasksSrv;
    ComPtr<ID3D11SamplerState> remoteVisualLowFpsSampler;
    ComPtr<ID3D11Buffer> unifiedSymbolMasks;
    ComPtr<ID3D11ShaderResourceView> unifiedSymbolMasksSrv;
    std::array<Slot, maximumSlots> slots;
    std::uint32_t slotCount = 0;
    DWORD ownerThread = 0;
    std::optional<ScreenCaptureDomain> activeDomain;
    std::array<float, maximumMetricCount> cpuMetrics{};
    std::array<float, maximumMetricCount> logicalMetrics{};
    std::array<RemoteVisualLowFpsFrameBinding, pbmodulation::kRemoteVisualTileCount> remoteVisualLowFpsFrameBindings{};
    std::array<UnifiedTileBinding, pbmodulation::kUnifiedVisualProfile.dataTileCount> unifiedTileBindings{};
    std::array<std::uint32_t, pbmodulation::kUnifiedFreshnessRegionCount * pbmodulation::kLocalDesktopTimingBits>
        unifiedExpectedFreshness{};
    std::array<std::uint8_t, pbmodulation::kUnifiedVisualProfile.dataTileCount> unifiedTileSamplingFailures{};
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> hard{};
    std::uint32_t remoteVisualLowFpsActiveTileCount = 0;
    pbdesktoplevels::ReferenceChannel evaluator;
    pbmodulation::UnifiedVisualCpuOracle unifiedOracle;
    pbdesktoplevels::EvaluationMode evaluationMode = pbdesktoplevels::EvaluationMode::DiagnosticTruth;
    mutable std::mutex snapshotMutex;
    DemodSnapshot snapshot;
};

namespace
{

DemodStatus ValidateOwner(Demodulator::Implementation& state, ID3D11DeviceContext* context, const DemodStage stage) noexcept
{
    if (GetCurrentThreadId() != state.ownerThread)
    {
        return DemodStatus::Failure(DemodError::WrongThread, stage);
    }
    return context != nullptr && SameComIdentity(context, state.context.Get()) ? DemodStatus{} :
        DemodStatus::Failure(DemodError::WrongDevice, stage);
}

void RetireSlot(Demodulator::Implementation& state, Demodulator::Implementation::Slot& slot, const bool failed) noexcept
{
    slot.inputSrv.Reset();
    slot.inputTexture.Reset();
    slot.frame = {};
    slot.binding = {};
    slot.remoteVisualLowFpsGeometry = {};
    slot.remoteVisualLowFpsPolicy = {};
    slot.unifiedBootstrap = {};
    slot.unifiedPolicy = {};
    slot.busy = false;
    slot.cancelled = false;
    slot.unbound = false;
    const std::lock_guard lock(state.snapshotMutex);
    if (state.snapshot.pendingFrames != 0)
    {
        state.snapshot.pendingFrames--;
    }
    if (failed)
    {
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.failedFrames);
    }
}

DemodStatus ValidateFrame(Demodulator::Implementation& state, const ScreenCaptureFrame& frame,
    const bool allowVariableSize) noexcept
{
    const auto& metadata = frame.metadata;
    const auto physicalWidth = static_cast<std::int64_t>(metadata.physicalRoi.right) - metadata.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(metadata.physicalRoi.bottom) - metadata.physicalRoi.top;
    const bool cursorProvenAbsent = metadata.sourceCursorState == pbcapturenormalize::CursorState::Excluded ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::SeparatePointer ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::KnownAbsent;
    if (frame.texture == nullptr || metadata.domain.captureEpoch == 0 || !NonzeroSourceId(metadata.domain) || metadata.captureObservation == 0 ||
        frame.metadata.sourceGeneration == 0 || frame.metadata.slotGeneration == 0 || frame.metadata.roiSize.width <= 0 ||
        frame.metadata.roiSize.height <= 0 || frame.metadata.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        frame.metadata.signalEncoding != CaptureSignalEncoding::SdrRgb || frame.metadata.hdr || !frame.metadata.isCursorExcluded ||
        !cursorProvenAbsent || physicalWidth != metadata.roiSize.width || physicalHeight != metadata.roiSize.height ||
        metadata.sourceContentSize.width <= 0 || metadata.sourceContentSize.height <= 0 ||
        metadata.sourceExtent.width <= 0 || metadata.sourceExtent.height <= 0 ||
        metadata.displayRotation < DXGI_MODE_ROTATION_IDENTITY || metadata.displayRotation > DXGI_MODE_ROTATION_ROTATE270 ||
        metadata.sourceTransform < DXGI_MODE_ROTATION_IDENTITY || metadata.sourceTransform > DXGI_MODE_ROTATION_ROTATE270 ||
        !SupportedSourceFormat(metadata.sourcePixelFormat) || metadata.timestamp.rawFrequency <= 0 ||
        metadata.timestamp.monotonic100ns < 0 || metadata.timestamp.arrivalQpc100ns < -1)
    {
        return DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Submission);
    }
    if ((!allowVariableSize && (frame.metadata.roiSize.width != 1920 || frame.metadata.roiSize.height != 1080)) ||
        frame.metadata.roiSize.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        frame.metadata.roiSize.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        return DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Submission);
    }
    if (!EqualLuid(frame.metadata.adapterLuid, state.snapshot.adapterLuid))
    {
        return DemodStatus::Failure(DemodError::AdapterMismatch, DemodStage::Submission);
    }
    D3D11_TEXTURE2D_DESC description{};
    frame.texture->GetDesc(&description);
    if (description.Width != static_cast<std::uint32_t>(metadata.roiSize.width) ||
        description.Height != static_cast<std::uint32_t>(metadata.roiSize.height) || description.MipLevels != 1 || description.ArraySize != 1 ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1 ||
        description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0 ||
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
    {
        return DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Submission);
    }
    ComPtr<ID3D11Device> textureDevice;
    frame.texture->GetDevice(&textureDevice);
    return SameComIdentity(textureDevice.Get(), state.device.Get()) ? DemodStatus{} :
        DemodStatus::Failure(DemodError::WrongDevice, DemodStage::Submission);
}

DemodStatus ValidateRemoteVisualLowFpsSubmission(const Demodulator::Implementation& state,
    const ScreenCaptureFrame& frame, const pbmodulation::LocalDesktopGeometry& geometry,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy,
    pbmodulation::LocalDesktopGeometry& samplingGeometry) noexcept
{
    const auto geometryStatus = pbmodulation::ResolveRemoteVisualLowFpsSamplingGeometry(geometry,
        frame.metadata.roiSize.width, frame.metadata.roiSize.height, samplingGeometry, policy);
    if (geometryStatus != pbmodulation::RemoteVisualLowFpsErasure::None)
    {
        return DemodStatus::Failure(geometryStatus == pbmodulation::RemoteVisualLowFpsErasure::FrameOutOfBounds ?
            DemodError::InvalidFrame : DemodError::InvalidBinding, DemodStage::Binding,
            static_cast<std::int32_t>(geometryStatus));
    }
    const auto tileSamples = pbprotocol::CheckedMultiplyUint64(state.remoteVisualLowFpsActiveTileCount,
        remoteVisualLowFpsSamplesPerTile);
    const auto totalSamples = tileSamples ?
        pbprotocol::CheckedAddUint64(tileSamples.Value(), remoteVisualLowFpsCalibrationSamples) : tileSamples;
    const auto maximumTexelReads = totalSamples ?
        pbprotocol::CheckedMultiplyUint64(totalSamples.Value(), remoteVisualLowFpsMaximumTexelsPerSample) : totalSamples;
    // maximumDataWorkUnits is a frozen CPU-reference policy measured in source
    // Pixel reads. A bilinear SampleLevel may consume four source texels, so the
    // GPU path conservatively enforces the same worst-case accounting boundary.
    if (!maximumTexelReads || policy.maximumDataWorkUnits < maximumTexelReads.Value())
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Binding,
            static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::WorkBudgetExceeded));
    }
    return {};
}

DemodStatus BuildRemoteVisualLowFpsFrameBindings(Demodulator::Implementation& state, const Binding& binding) noexcept
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        if (!pbmodulation::GetRemoteVisualTileMapping(physical, mapping))
        {
            return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
        }
        auto& frameBinding = state.remoteVisualLowFpsFrameBindings[physical];
        frameBinding.logicalBits.fill(pbmodulation::kRemoteVisualLowFpsCodedBits);
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            bool expectedOne = false;
            if (!pbmodulation::GetRemoteVisualFreshnessBit(binding.sessionTag, binding.frameSequence, physical, expectedOne))
            {
                return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
            }
            frameBinding.logicalBits[0] = static_cast<std::uint32_t>(expectedOne);
        }
        else if (mapping.role == pbmodulation::RemoteVisualTileRole::Data)
        {
            for (std::uint32_t plane = 0; plane < pbmodulation::kRemoteVisualLowFpsBitsPerTile; plane++)
            {
                frameBinding.logicalBits[plane] =
                    pbmodulation::GetRemoteVisualLowFpsLogicalBit(mapping.dataOrdinal, plane, binding.frameSequence);
            }
        }
    }
    return {};
}

DemodStatus ValidateUnifiedSubmission(const ScreenCaptureFrame& frame,
    const pbmodulation::LocalDesktopObservation& bootstrap,
    const pbmodulation::UnifiedVisualDecodePolicy& policy,
    pbmodulation::LocalDesktopGeometry& samplingGeometry) noexcept
{
    if (!bootstrap.IsAccepted() || !pbmodulation::ValidateUnifiedVisualDecodePolicy(policy) ||
        !std::isfinite(bootstrap.blackLevel) || !std::isfinite(bootstrap.whiteLevel) ||
        bootstrap.whiteLevel <= bootstrap.blackLevel)
    {
        return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
    }
    if (!pbmodulation::ResolveUnifiedVisualSamplingGeometry(bootstrap.geometry,
        static_cast<std::uint32_t>(frame.metadata.roiSize.width),
        static_cast<std::uint32_t>(frame.metadata.roiSize.height), policy, samplingGeometry))
    {
        return DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Binding,
            static_cast<std::int32_t>(pbmodulation::UnifiedErasureReason::CanvasClipped));
    }
    return {};
}

DemodStatus BuildUnifiedFrameBindings(Demodulator::Implementation& state, const Binding& binding,
    const std::span<const std::byte> bootstrapRecord) noexcept
{
    constexpr std::uint32_t invalidMetric = static_cast<std::uint32_t>(pbmodulation::kUnifiedSoftMetricCount);
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < pbmodulation::kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const pbmodulation::UnifiedDataTile tile = pbmodulation::GetUnifiedDataTile(tileOrdinal);
        if (!tile.valid)
        {
            return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
        }
        UnifiedTileBinding& destination = state.unifiedTileBindings[tileOrdinal];
        destination = {};
        destination.originX = tile.bounds.x;
        destination.originY = tile.bounds.y;
        destination.lumaBits.fill(invalidMetric);
        destination.chromaBits.fill(invalidMetric);
        for (std::uint8_t bitPlane = 0; bitPlane < destination.lumaBits.size(); bitPlane++)
        {
            const auto logical = pbmodulation::GetUnifiedLogicalCarrierBit(
                {true, pbmodulation::UnifiedCarrier::Luma, tileOrdinal, bitPlane}, binding.frameSequence);
            if (logical.valid)
            {
                const auto capacity = pbmodulation::GetUnifiedLaneCapacity(logical.lane);
                destination.lumaBits[bitPlane] = capacity.firstCodewordSlot *
                    pbmodulation::kUnifiedVisualProfile.innerCodewordBits + logical.logicalBit;
            }
        }
        for (std::uint8_t bitPlane = 0; bitPlane < destination.chromaBits.size(); bitPlane++)
        {
            const auto logical = pbmodulation::GetUnifiedLogicalCarrierBit(
                {true, pbmodulation::UnifiedCarrier::Chroma, tileOrdinal, bitPlane}, binding.frameSequence);
            if (logical.valid)
            {
                const auto capacity = pbmodulation::GetUnifiedLaneCapacity(logical.lane);
                destination.chromaBits[bitPlane] = capacity.firstCodewordSlot *
                    pbmodulation::kUnifiedVisualProfile.innerCodewordBits + logical.logicalBit;
            }
        }
    }
    for (std::uint32_t region = 0; region < pbmodulation::kUnifiedFreshnessRegionCount; region++)
    {
        std::array<std::uint8_t, pbmodulation::kLocalDesktopTimingBits> expected{};
        if (!pbmodulation::BuildUnifiedFreshnessBits(bootstrapRecord, region, expected))
        {
            return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
        }
        for (std::size_t bit = 0; bit < expected.size(); bit++)
        {
            state.unifiedExpectedFreshness[static_cast<std::size_t>(region) * expected.size() + bit] = expected[bit];
        }
    }
    return {};
}

DemodStatus ClassifyNativeFailure(Demodulator::Implementation& state, const HRESULT native,
    const DemodStage stage, const DemodError fallback) noexcept
{
    const HRESULT removed = state.device->GetDeviceRemovedReason();
    if (native == DXGI_ERROR_DEVICE_REMOVED || native == DXGI_ERROR_DEVICE_RESET || FAILED(removed))
    {
        return DemodStatus::Failure(DemodError::DeviceLost, stage, FAILED(removed) ? removed : native);
    }
    return DemodStatus::Failure(fallback, stage, native);
}

} // namespace

Demodulator::Demodulator() noexcept = default;
Demodulator::Demodulator(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}
Demodulator::Demodulator(Demodulator&&) noexcept = default;
Demodulator& Demodulator::operator=(Demodulator&&) noexcept = default;
Demodulator::~Demodulator() = default;

DemodStatus CalculateDemodulatorResidentBytes(const DemodConfig& config, std::uint64_t& output) noexcept
{
    if (config.readbackSlotCount < minimumSlots || config.readbackSlotCount > maximumSlots ||
        (config.evaluationMode != pbdesktoplevels::EvaluationMode::DiagnosticTruth &&
         config.evaluationMode != pbdesktoplevels::EvaluationMode::Transport))
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Configuration);
    }
    const auto metricPairBytes = pbprotocol::CheckedMultiplyUint64(maximumMetricBytes, 2);
    const auto calibrationPairBytes = pbprotocol::CheckedMultiplyUint64(calibrationBytes, 2);
    const auto freshnessSummaryPairBytes = pbprotocol::CheckedMultiplyUint64(remoteVisualLowFpsFreshnessSummaryBytes, 2);
    const auto unifiedTileSamplingPairBytes = pbprotocol::CheckedMultiplyUint64(unifiedTileSamplingBytes, 2);
    const auto unifiedFreshnessPairBytes = pbprotocol::CheckedMultiplyUint64(unifiedFreshnessBytes, 2);
    const auto unifiedPhasePairBytes = pbprotocol::CheckedMultiplyUint64(unifiedPhaseBytes, 2);
    if (!metricPairBytes || !calibrationPairBytes || !freshnessSummaryPairBytes ||
        !unifiedTileSamplingPairBytes || !unifiedFreshnessPairBytes || !unifiedPhasePairBytes)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Configuration);
    }
    const auto perSlotFirst = pbprotocol::CheckedAddUint64(metricPairBytes.Value(), calibrationPairBytes.Value());
    const auto perSlotSecond = perSlotFirst ?
        pbprotocol::CheckedAddUint64(perSlotFirst.Value(), freshnessSummaryPairBytes.Value()) : perSlotFirst;
    const auto perSlotThird = perSlotSecond ?
        pbprotocol::CheckedAddUint64(perSlotSecond.Value(), remoteVisualLowFpsFrameBindingBytes) : perSlotSecond;
    const auto perSlotFourth = perSlotThird ?
        pbprotocol::CheckedAddUint64(perSlotThird.Value(), unifiedTileBindingBytes) : perSlotThird;
    const auto perSlotFifth = perSlotFourth ?
        pbprotocol::CheckedAddUint64(perSlotFourth.Value(), unifiedExpectedFreshnessBytes) : perSlotFourth;
    const auto perSlotSixth = perSlotFifth ?
        pbprotocol::CheckedAddUint64(perSlotFifth.Value(), unifiedTileSamplingPairBytes.Value()) : perSlotFifth;
    const auto perSlotSeventh = perSlotSixth ?
        pbprotocol::CheckedAddUint64(perSlotSixth.Value(), unifiedFreshnessPairBytes.Value()) : perSlotSixth;
    const auto perSlotEighth = perSlotSeventh ?
        pbprotocol::CheckedAddUint64(perSlotSeventh.Value(), unifiedPhasePairBytes.Value()) : perSlotSeventh;
    const auto perSlotBytes = perSlotEighth ?
        pbprotocol::CheckedAddUint64(perSlotEighth.Value(), sizeof(FrameConstants)) : perSlotEighth;
    if (!perSlotBytes)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Configuration);
    }
    const auto allSlotBytes = pbprotocol::CheckedMultiplyUint64(perSlotBytes.Value(), config.readbackSlotCount);
    const auto residentFirst = allSlotBytes ? pbprotocol::CheckedAddUint64(allSlotBytes.Value(), maximumMetricBytes) : allSlotBytes;
    const auto residentSecond = residentFirst ? pbprotocol::CheckedAddUint64(residentFirst.Value(), maximumMetricBytes) : residentFirst;
    const auto residentThird = residentSecond ?
        pbprotocol::CheckedAddUint64(residentSecond.Value(), pbmodulation::kDesktopLevelsMaximumDataBytes) : residentSecond;
    const auto residentFourth = residentThird ?
        pbprotocol::CheckedAddUint64(residentThird.Value(), pbdesktoplevels::kProcessingReservationBytes) : residentThird;
    const auto residentFifth = residentFourth ?
        pbprotocol::CheckedAddUint64(residentFourth.Value(), remoteVisualLowFpsFrameBindingBytes) : residentFourth;
    const auto residentSixth = residentFifth ?
        pbprotocol::CheckedAddUint64(residentFifth.Value(), remoteVisualLowFpsTileMappingBytes) : residentFifth;
    const auto residentSeventh = residentSixth ?
        pbprotocol::CheckedAddUint64(residentSixth.Value(), remoteVisualLowFpsSymbolMaskBytes) : residentSixth;
    const auto residentEighth = residentSeventh ?
        pbprotocol::CheckedAddUint64(residentSeventh.Value(), unifiedTileBindingBytes) : residentSeventh;
    const auto residentNinth = residentEighth ?
        pbprotocol::CheckedAddUint64(residentEighth.Value(), unifiedExpectedFreshnessBytes) : residentEighth;
    const auto residentTenth = residentNinth ? pbprotocol::CheckedAddUint64(residentNinth.Value(),
        pbmodulation::kUnifiedVisualProfile.dataTileCount * sizeof(std::uint8_t)) : residentNinth;
    const auto residentEleventh = residentTenth ?
        pbprotocol::CheckedAddUint64(residentTenth.Value(), unifiedSymbolMaskBytes) : residentTenth;
    const auto residentTwelfth = residentEleventh ? pbprotocol::CheckedAddUint64(
        residentEleventh.Value(), pbmodulation::UnifiedVisualCpuOracle::RequiredBytes()) : residentEleventh;
    const auto residentBytesResult = residentTwelfth ?
        pbprotocol::CheckedAddUint64(residentTwelfth.Value(), 1024ULL * 1024) : residentTwelfth;
    if (!residentBytesResult || residentBytesResult.Value() > config.maximumResidentBytes)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Configuration);
    }
    output = residentBytesResult.Value();
    return {};
}

DemodStatus Demodulator::Create(ID3D11Device* device, const DemodConfig& config, std::unique_ptr<Demodulator>& output) noexcept
{
    if (device == nullptr)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Configuration);
    }
    std::uint64_t residentBytes = 0;
    const auto budgetStatus = CalculateDemodulatorResidentBytes(config, residentBytes);
    if (!budgetStatus)
    {
        return budgetStatus;
    }
    try
    {
        auto state = std::make_unique<Implementation>();
        state->device = device;
        device->GetImmediateContext(&state->context);
        if (!state->context)
        {
            return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Device);
        }
        state->ownerThread = GetCurrentThreadId();
        state->slotCount = config.readbackSlotCount;
        auto evaluator = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        if (!evaluator)
        {
            return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Resource);
        }
        state->evaluator = std::move(evaluator).Value();
        auto unifiedOracle = pbmodulation::UnifiedVisualCpuOracle::Create(
            pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
        if (!unifiedOracle)
        {
            return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Resource);
        }
        state->unifiedOracle = std::move(unifiedOracle).Value();
        state->evaluationMode = config.evaluationMode;
        DemodStatus status = GetAdapterLuid(device, state->snapshot.adapterLuid);
        if (!status)
        {
            return status;
        }
        std::array<RemoteVisualLowFpsTileMapping, pbmodulation::kRemoteVisualTileCount> tileMappings{};
        std::array<bool, pbmodulation::kRemoteVisualFreshnessRegionCount> eligibleRegions{};
        for (std::uint32_t physical = 0; physical < tileMappings.size(); physical++)
        {
            pbmodulation::LocalDesktopRegion region;
            pbmodulation::RemoteVisualTileMapping mapping;
            if (!pbmodulation::GetRemoteVisualTile(physical, region) ||
                !pbmodulation::GetRemoteVisualTileMapping(physical, mapping) ||
                mapping.regionId >= pbmodulation::kRemoteVisualFreshnessRegionCount)
            {
                return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Resource);
            }
            tileMappings[physical] = {region.x, region.y, static_cast<std::uint32_t>(mapping.role), mapping.regionId};
            state->remoteVisualLowFpsActiveTileCount +=
                static_cast<std::uint32_t>(mapping.role != pbmodulation::RemoteVisualTileRole::Unused);
            eligibleRegions[mapping.regionId] = eligibleRegions[mapping.regionId] ||
                mapping.role != pbmodulation::RemoteVisualTileRole::Unused;
        }
        if (std::ranges::count(eligibleRegions, true) != pbmodulation::kRemoteVisualEligibleFreshnessRegions)
        {
            return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Resource);
        }
        std::array<std::uint32_t, pbmodulation::kRemoteVisualLowFpsSymbolMasks.size()> symbolMasks{};
        std::transform(pbmodulation::kRemoteVisualLowFpsSymbolMasks.begin(), pbmodulation::kRemoteVisualLowFpsSymbolMasks.end(),
            symbolMasks.begin(), [](const std::uint16_t mask) { return static_cast<std::uint32_t>(mask); });
        status = CreateImmutableStructuredBuffer(device, remoteVisualLowFpsTileMappingBytes,
            sizeof(RemoteVisualLowFpsTileMapping), tileMappings.data(), state->remoteVisualLowFpsTileMappings,
            state->remoteVisualLowFpsTileMappingsSrv);
        if (status)
        {
            status = CreateImmutableStructuredBuffer(device, remoteVisualLowFpsSymbolMaskBytes, sizeof(std::uint32_t),
                symbolMasks.data(), state->remoteVisualLowFpsSymbolMasks, state->remoteVisualLowFpsSymbolMasksSrv);
        }
        std::array<std::uint32_t, pbmodulation::kUnifiedSymbolMasksByLabel.size()> unifiedSymbolMasks{};
        std::transform(pbmodulation::kUnifiedSymbolMasksByLabel.begin(), pbmodulation::kUnifiedSymbolMasksByLabel.end(),
            unifiedSymbolMasks.begin(), [](const std::uint16_t mask) { return static_cast<std::uint32_t>(mask); });
        if (status)
        {
            status = CreateImmutableStructuredBuffer(device, unifiedSymbolMaskBytes, sizeof(std::uint32_t),
                unifiedSymbolMasks.data(), state->unifiedSymbolMasks, state->unifiedSymbolMasksSrv);
        }
        if (!status)
        {
            return status;
        }
        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDescription.MinLOD = 0;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        const HRESULT samplerStatus = device->CreateSamplerState(&samplerDescription, &state->remoteVisualLowFpsSampler);
        if (FAILED(samplerStatus))
        {
            return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, samplerStatus);
        }
        struct ShaderRequest
        {
            const char* source;
            std::size_t sourceBytes;
            const char* entryPoint;
            ComPtr<ID3D11ComputeShader>* output;
        };
        const std::array<ShaderRequest, 12> shaders{{
            {detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
                "CalibrateChromaCS", std::addressof(state->calibrateChroma)},
            {detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
                "CalibrateLevelsCS", std::addressof(state->calibrateLevels)},
            {detail::kRemoteVisualLowFpsComputeShader, sizeof(detail::kRemoteVisualLowFpsComputeShader) - 1,
                "CalibrateRemoteVisualLowFpsCS", std::addressof(state->calibrateRemoteVisualLowFps)},
            {detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
                "DemodShapeChromaCS", std::addressof(state->demodShapeChroma)},
            {detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
                "DemodDesktopLevelsCS", std::addressof(state->demodDesktopLevels)},
            {detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
                "DemodRemoteVisualCS", std::addressof(state->demodRemoteVisual)},
            {detail::kRemoteVisualLowFpsComputeShader, sizeof(detail::kRemoteVisualLowFpsComputeShader) - 1,
                "DemodRemoteVisualLowFpsFreshnessCS", std::addressof(state->demodRemoteVisualLowFpsFreshness)},
            {detail::kRemoteVisualLowFpsComputeShader, sizeof(detail::kRemoteVisualLowFpsComputeShader) - 1,
                "DemodRemoteVisualLowFpsCS", std::addressof(state->demodRemoteVisualLowFps)},
            {detail::kUnifiedVisualComputeShader, sizeof(detail::kUnifiedVisualComputeShader) - 1,
                "CalibrateUnifiedCS", std::addressof(state->calibrateUnified)},
            {detail::kUnifiedVisualComputeShader, sizeof(detail::kUnifiedVisualComputeShader) - 1,
                "EvaluateUnifiedFreshnessCS", std::addressof(state->evaluateUnifiedFreshness)},
            {detail::kUnifiedVisualComputeShader, sizeof(detail::kUnifiedVisualComputeShader) - 1,
                "EvaluateUnifiedPhaseCS", std::addressof(state->evaluateUnifiedPhase)},
            {detail::kUnifiedVisualComputeShader, sizeof(detail::kUnifiedVisualComputeShader) - 1,
                "DemodUnifiedCS", std::addressof(state->demodUnified)}}};
        for (const auto& entry : shaders)
        {
            status = CompileShader(device, entry.source, entry.sourceBytes, entry.entryPoint, *entry.output);
            if (!status)
            {
                return status;
            }
        }
        for (std::uint32_t index = 0; index < state->slotCount; index++)
        {
            auto& slot = state->slots[index];
            status = CreateStructuredBuffer(device, maximumMetricBytes, sizeof(float), D3D11_BIND_UNORDERED_ACCESS,
                D3D11_USAGE_DEFAULT, 0, slot.metrics);
            if (status)
            {
                status = CreateStructuredBuffer(device, maximumMetricBytes, sizeof(float), 0, D3D11_USAGE_STAGING,
                    D3D11_CPU_ACCESS_READ, slot.metricsStaging);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, calibrationBytes, sizeof(float) * 4,
                    D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, slot.calibration);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, calibrationBytes, sizeof(float) * 4, 0, D3D11_USAGE_STAGING,
                    D3D11_CPU_ACCESS_READ, slot.calibrationStaging);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, remoteVisualLowFpsFrameBindingBytes,
                    sizeof(RemoteVisualLowFpsFrameBinding), D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0,
                    slot.remoteVisualLowFpsFrameBindings);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, remoteVisualLowFpsFreshnessSummaryBytes,
                    sizeof(RemoteVisualLowFpsFreshnessSummary), D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0,
                    slot.remoteVisualLowFpsFreshnessSummary);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, remoteVisualLowFpsFreshnessSummaryBytes,
                    sizeof(RemoteVisualLowFpsFreshnessSummary), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ,
                    slot.remoteVisualLowFpsFreshnessSummaryStaging);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedTileBindingBytes, sizeof(UnifiedTileBinding),
                    D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, slot.unifiedTileBindings);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedExpectedFreshnessBytes, sizeof(std::uint32_t),
                    D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, slot.unifiedExpectedFreshness);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedTileSamplingBytes, sizeof(std::uint32_t),
                    D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, slot.unifiedTileSamplingFailures);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedTileSamplingBytes, sizeof(std::uint32_t), 0,
                    D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, slot.unifiedTileSamplingFailuresStaging);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedFreshnessBytes, sizeof(float) * 4,
                    D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, slot.unifiedFreshness);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedFreshnessBytes, sizeof(float) * 4, 0,
                    D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, slot.unifiedFreshnessStaging);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedPhaseBytes, sizeof(float) * 4,
                    D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, slot.unifiedPhase);
            }
            if (status)
            {
                status = CreateStructuredBuffer(device, unifiedPhaseBytes, sizeof(float) * 4, 0,
                    D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, slot.unifiedPhaseStaging);
            }
            if (!status)
            {
                return status;
            }
            HRESULT native = device->CreateUnorderedAccessView(slot.metrics.Get(), nullptr, &slot.metricsUav);
            if (SUCCEEDED(native))
            {
                native = device->CreateUnorderedAccessView(slot.calibration.Get(), nullptr, &slot.calibrationUav);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateShaderResourceView(slot.calibration.Get(), nullptr, &slot.calibrationSrv);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateShaderResourceView(slot.remoteVisualLowFpsFrameBindings.Get(), nullptr,
                    &slot.remoteVisualLowFpsFrameBindingsSrv);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateUnorderedAccessView(slot.remoteVisualLowFpsFreshnessSummary.Get(), nullptr,
                    &slot.remoteVisualLowFpsFreshnessSummaryUav);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateShaderResourceView(slot.unifiedTileBindings.Get(), nullptr,
                    &slot.unifiedTileBindingsSrv);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateShaderResourceView(slot.unifiedExpectedFreshness.Get(), nullptr,
                    &slot.unifiedExpectedFreshnessSrv);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateUnorderedAccessView(slot.unifiedTileSamplingFailures.Get(), nullptr,
                    &slot.unifiedTileSamplingFailuresUav);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateUnorderedAccessView(slot.unifiedFreshness.Get(), nullptr,
                    &slot.unifiedFreshnessUav);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateUnorderedAccessView(slot.unifiedPhase.Get(), nullptr,
                    &slot.unifiedPhaseUav);
            }
            if (FAILED(native))
            {
                return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, native);
            }
            status = CreateConstantBuffer(device, slot.constants);
            if (!status)
            {
                return status;
            }
            D3D11_QUERY_DESC query{};
            query.Query = D3D11_QUERY_EVENT;
            native = device->CreateQuery(&query, &slot.completion);
            if (SUCCEEDED(native))
            {
                query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                native = device->CreateQuery(&query, &slot.timestampDisjoint);
            }
            if (SUCCEEDED(native))
            {
                query.Query = D3D11_QUERY_TIMESTAMP;
                native = device->CreateQuery(&query, &slot.timestampStart);
            }
            if (SUCCEEDED(native))
            {
                native = device->CreateQuery(&query, &slot.timestampEnd);
            }
            if (FAILED(native))
            {
                return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, native);
            }
        }
        state->snapshot.residentBytes = residentBytes;
        auto candidate = std::unique_ptr<Demodulator>(new (std::nothrow) Demodulator(std::move(state)));
        if (!candidate)
        {
            return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Resource);
        }
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Resource);
    }
}

namespace
{
DemodStatus SubmitInternal(Demodulator::Implementation& state, const ScreenCaptureFrame& frame,
    ID3D11DeviceContext* const context, const std::span<const std::byte> bootstrapRecord,
    const pbmodulation::LocalDesktopGeometry* const remoteVisualLowFpsGeometry,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy* const remoteVisualLowFpsPolicy,
    const pbmodulation::LocalDesktopObservation* const unifiedBootstrap,
    const pbmodulation::UnifiedVisualDecodePolicy* const unifiedPolicy, DemodSubmission& output) noexcept
{
    auto status = ValidateOwner(state, context, DemodStage::Submission);
    if (!status)
    {
        return status;
    }
    {
        const std::lock_guard lock(state.snapshotMutex);
        if (state.snapshot.shutdown)
        {
            return DemodStatus::Failure(DemodError::ShutdownRequired, DemodStage::Submission);
        }
    }
    Binding binding;
    status = ParseBinding(bootstrapRecord, binding);
    if (!status)
    {
        return status;
    }
    const bool remoteVisualLowFps = binding.mode == ProfileMode::RemoteVisualLowFps;
    const bool remoteVisualLowFpsRequested = remoteVisualLowFpsGeometry != nullptr && remoteVisualLowFpsPolicy != nullptr;
    const bool unifiedVisual = binding.mode == ProfileMode::UnifiedVisual;
    const bool unifiedVisualRequested = unifiedBootstrap != nullptr && unifiedPolicy != nullptr;
    if (remoteVisualLowFps != remoteVisualLowFpsRequested || unifiedVisual != unifiedVisualRequested ||
        (remoteVisualLowFpsRequested && unifiedVisualRequested))
    {
        return DemodStatus::Failure(DemodError::UnsupportedProfile, DemodStage::Binding);
    }
    status = ValidateFrame(state, frame, remoteVisualLowFps || unifiedVisual);
    if (!status)
    {
        return status;
    }
    pbmodulation::LocalDesktopGeometry remoteVisualLowFpsSamplingGeometry;
    if (remoteVisualLowFps)
    {
        status = ValidateRemoteVisualLowFpsSubmission(state, frame, *remoteVisualLowFpsGeometry,
            *remoteVisualLowFpsPolicy, remoteVisualLowFpsSamplingGeometry);
        if (!status)
        {
            return status;
        }
    }
    pbmodulation::LocalDesktopGeometry unifiedSamplingGeometry;
    if (unifiedVisual)
    {
        status = ValidateUnifiedSubmission(frame, *unifiedBootstrap, *unifiedPolicy, unifiedSamplingGeometry);
        if (!status)
        {
            return status;
        }
    }
    if (!state.activeDomain)
    {
        state.activeDomain = frame.metadata.domain;
    }
    else if (*state.activeDomain != frame.metadata.domain)
    {
        return DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Submission);
    }
    std::uint32_t slotIndex = state.slotCount;
    for (std::uint32_t index = 0; index < state.slotCount; index++)
    {
        if (!state.slots[index].busy)
        {
            slotIndex = index;
            break;
        }
    }
    if (slotIndex == state.slotCount)
    {
        return DemodStatus::Failure(DemodError::Busy, DemodStage::Submission);
    }
    auto& slot = state.slots[slotIndex];
    if (slot.generation == UINT64_MAX)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Submission);
    }
    if (remoteVisualLowFps)
    {
        status = BuildRemoteVisualLowFpsFrameBindings(state, binding);
        if (!status)
        {
            return status;
        }
    }
    else if (unifiedVisual)
    {
        status = BuildUnifiedFrameBindings(state, binding, bootstrapRecord);
        if (!status)
        {
            return status;
        }
    }
    ComPtr<ID3D11ShaderResourceView> inputSrv;
    const HRESULT createSrv = state.device->CreateShaderResourceView(frame.texture, nullptr, &inputSrv);
    if (FAILED(createSrv))
    {
        return ClassifyNativeFailure(state, createSrv, DemodStage::Resource, DemodError::NativeFailure);
    }
    FrameConstants constants{};
    constants.mode = static_cast<std::uint32_t>(binding.mode);
    constants.tilePixels = binding.tilePixels;
    constants.tileCount = binding.tileCount;
    constants.rowTiles = binding.rowTiles;
    constants.interleavePhase = binding.interleavePhase;
    constants.metricCount = binding.metricCount;
    constants.reserved0 = binding.codedMetricCount;
    if (remoteVisualLowFps)
    {
        constants.originX = static_cast<float>(remoteVisualLowFpsSamplingGeometry.originX);
        constants.originY = static_cast<float>(remoteVisualLowFpsSamplingGeometry.originY);
        constants.scaleX = static_cast<float>(remoteVisualLowFpsSamplingGeometry.scaleX);
        constants.scaleY = static_cast<float>(remoteVisualLowFpsSamplingGeometry.scaleY);
        constants.sourceTexelPitchX = 1.0f / static_cast<float>(frame.metadata.roiSize.width);
        constants.sourceTexelPitchY = 1.0f / static_cast<float>(frame.metadata.roiSize.height);
        constants.minimumEndpointSeparation = static_cast<float>(remoteVisualLowFpsPolicy->minimumEndpointSeparation);
        constants.maximumPilotVariance = static_cast<float>(remoteVisualLowFpsPolicy->maximumPilotStandardDeviation *
            remoteVisualLowFpsPolicy->maximumPilotStandardDeviation);
        constants.maximumPilotSpatialDeviation = static_cast<float>(remoteVisualLowFpsPolicy->maximumPilotSpatialDeviation);
        constants.minimumSymbolRms = static_cast<float>(remoteVisualLowFpsPolicy->minimumSymbolRms);
        constants.maximumSymbolResidual = static_cast<float>(remoteVisualLowFpsPolicy->maximumSymbolResidual);
        constants.minimumSymbolMargin = static_cast<float>(remoteVisualLowFpsPolicy->minimumSymbolMargin);
        constants.minimumFreshnessMetric = static_cast<float>(remoteVisualLowFpsPolicy->minimumFreshnessMetric);
        constants.sourceWidth = static_cast<std::uint32_t>(frame.metadata.roiSize.width);
        constants.sourceHeight = static_cast<std::uint32_t>(frame.metadata.roiSize.height);
        constants.freshnessRegionCount = pbmodulation::kRemoteVisualFreshnessRegionCount;
    }
    else if (unifiedVisual)
    {
        constants.originX = static_cast<float>(unifiedSamplingGeometry.originX);
        constants.originY = static_cast<float>(unifiedSamplingGeometry.originY);
        constants.scaleX = static_cast<float>(unifiedSamplingGeometry.scaleX);
        constants.scaleY = static_cast<float>(unifiedSamplingGeometry.scaleY);
        constants.sourceTexelPitchX = 1.0f / static_cast<float>(frame.metadata.roiSize.width);
        constants.sourceTexelPitchY = 1.0f / static_cast<float>(frame.metadata.roiSize.height);
        constants.minimumEndpointSeparation = static_cast<float>(unifiedPolicy->minimumLumaLevelGap);
        constants.maximumPilotVariance = static_cast<float>(
            unifiedPolicy->maximumPilotDeviation * unifiedPolicy->maximumPilotDeviation);
        constants.maximumPilotSpatialDeviation = static_cast<float>(unifiedPolicy->minimumChromaSeparation);
        constants.minimumSymbolRms = static_cast<float>(unifiedPolicy->maximumPhasePilotResidual);
        constants.maximumSymbolResidual = static_cast<float>(unifiedPolicy->maximumTimingBitErrorFraction);
        constants.minimumSymbolMargin = static_cast<float>(unifiedPolicy->locator.maximumTimingResidual);
        constants.minimumFreshnessMetric = static_cast<float>(unifiedPolicy->minimumDecisionMetric);
        constants.sourceWidth = static_cast<std::uint32_t>(frame.metadata.roiSize.width);
        constants.sourceHeight = static_cast<std::uint32_t>(frame.metadata.roiSize.height);
        constants.freshnessRegionCount = pbmodulation::kUnifiedFreshnessRegionCount;
        constants.bootstrapBlackLevel = static_cast<float>(unifiedBootstrap->blackLevel);
        constants.bootstrapWhiteLevel = static_cast<float>(unifiedBootstrap->whiteLevel);
    }
    context->Begin(slot.timestampDisjoint.Get());
    context->End(slot.timestampStart.Get());
    context->UpdateSubresource(slot.constants.Get(), 0, nullptr, &constants, 0, 0);
    if (remoteVisualLowFps)
    {
        context->UpdateSubresource(slot.remoteVisualLowFpsFrameBindings.Get(), 0, nullptr,
            state.remoteVisualLowFpsFrameBindings.data(), 0, 0);
        const UINT clearValues[4]{};
        context->ClearUnorderedAccessViewUint(slot.metricsUav.Get(), clearValues);
        context->ClearUnorderedAccessViewUint(slot.remoteVisualLowFpsFreshnessSummaryUav.Get(), clearValues);
    }
    else if (unifiedVisual)
    {
        context->UpdateSubresource(slot.unifiedTileBindings.Get(), 0, nullptr,
            state.unifiedTileBindings.data(), 0, 0);
        context->UpdateSubresource(slot.unifiedExpectedFreshness.Get(), 0, nullptr,
            state.unifiedExpectedFreshness.data(), 0, 0);
        const UINT clearValues[4]{};
        context->ClearUnorderedAccessViewUint(slot.metricsUav.Get(), clearValues);
        context->ClearUnorderedAccessViewUint(slot.unifiedTileSamplingFailuresUav.Get(), clearValues);
        context->ClearUnorderedAccessViewUint(slot.unifiedFreshnessUav.Get(), clearValues);
        context->ClearUnorderedAccessViewUint(slot.unifiedPhaseUav.Get(), clearValues);
    }
    const float clearCalibration[4]{};
    context->ClearUnorderedAccessViewFloat(slot.calibrationUav.Get(), clearCalibration);
    ID3D11Buffer* constantBuffers[]{slot.constants.Get()};
    context->CSSetConstantBuffers(0, 1, constantBuffers);
    ID3D11ShaderResourceView* calibrationInputs[]{inputSrv.Get(), nullptr, nullptr, nullptr, nullptr, nullptr};
    context->CSSetShaderResources(0, 6, calibrationInputs);
    ID3D11UnorderedAccessView* calibrationOutputs[]{slot.calibrationUav.Get(), nullptr, nullptr, nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 5, calibrationOutputs, nullptr);
    ID3D11ComputeShader* const calibrationShader = binding.mode == ProfileMode::ShapeChroma ? state.calibrateChroma.Get() :
        remoteVisualLowFps ? state.calibrateRemoteVisualLowFps.Get() :
            unifiedVisual ? state.calibrateUnified.Get() : state.calibrateLevels.Get();
    context->CSSetShader(calibrationShader, nullptr, 0);
    context->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* noOutputs[]{nullptr, nullptr, nullptr, nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 5, noOutputs, nullptr);
    ID3D11ShaderResourceView* demodInputs[]{inputSrv.Get(), slot.calibrationSrv.Get(),
        remoteVisualLowFps ? state.remoteVisualLowFpsTileMappingsSrv.Get() : nullptr,
        remoteVisualLowFps ? slot.remoteVisualLowFpsFrameBindingsSrv.Get() :
            unifiedVisual ? slot.unifiedTileBindingsSrv.Get() : nullptr,
        remoteVisualLowFps ? state.remoteVisualLowFpsSymbolMasksSrv.Get() :
            unifiedVisual ? state.unifiedSymbolMasksSrv.Get() : nullptr,
        unifiedVisual ? slot.unifiedExpectedFreshnessSrv.Get() : nullptr};
    context->CSSetShaderResources(0, 6, demodInputs);
    if (remoteVisualLowFps)
    {
        ID3D11SamplerState* samplers[]{state.remoteVisualLowFpsSampler.Get()};
        context->CSSetSamplers(0, 1, samplers);
        ID3D11UnorderedAccessView* freshnessOutputs[]{nullptr, nullptr, slot.remoteVisualLowFpsFreshnessSummaryUav.Get()};
        context->CSSetUnorderedAccessViews(0, 3, freshnessOutputs, nullptr);
        context->CSSetShader(state.demodRemoteVisualLowFpsFreshness.Get(), nullptr, 0);
        context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
        ID3D11UnorderedAccessView* dataOutputs[]{nullptr, slot.metricsUav.Get(), slot.remoteVisualLowFpsFreshnessSummaryUav.Get()};
        context->CSSetUnorderedAccessViews(0, 3, dataOutputs, nullptr);
        context->CSSetShader(state.demodRemoteVisualLowFps.Get(), nullptr, 0);
        context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
    }
    else if (unifiedVisual)
    {
        ID3D11UnorderedAccessView* freshnessOutputs[]{nullptr, nullptr, nullptr,
            slot.unifiedFreshnessUav.Get(), nullptr};
        context->CSSetUnorderedAccessViews(0, 5, freshnessOutputs, nullptr);
        context->CSSetShader(state.evaluateUnifiedFreshness.Get(), nullptr, 0);
        context->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* phaseOutputs[]{nullptr, nullptr, nullptr, nullptr,
            slot.unifiedPhaseUav.Get()};
        context->CSSetUnorderedAccessViews(0, 5, phaseOutputs, nullptr);
        context->CSSetShader(state.evaluateUnifiedPhase.Get(), nullptr, 0);
        context->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* dataOutputs[]{nullptr, slot.metricsUav.Get(),
            slot.unifiedTileSamplingFailuresUav.Get(), nullptr, nullptr};
        context->CSSetUnorderedAccessViews(0, 5, dataOutputs, nullptr);
        context->CSSetShader(state.demodUnified.Get(), nullptr, 0);
        context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
    }
    else
    {
        ID3D11UnorderedAccessView* demodOutputs[]{nullptr, slot.metricsUav.Get(), nullptr};
        context->CSSetUnorderedAccessViews(0, 3, demodOutputs, nullptr);
        ID3D11ComputeShader* const demodShader = binding.mode == ProfileMode::ShapeChroma ? state.demodShapeChroma.Get() :
            binding.mode == ProfileMode::RemoteVisual ? state.demodRemoteVisual.Get() : state.demodDesktopLevels.Get();
        context->CSSetShader(demodShader, nullptr, 0);
        context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
    }
    context->CSSetUnorderedAccessViews(0, 5, noOutputs, nullptr);
    ID3D11ShaderResourceView* noInputs[]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    context->CSSetShaderResources(0, 6, noInputs);
    ID3D11SamplerState* noSamplers[]{nullptr};
    context->CSSetSamplers(0, 1, noSamplers);
    ID3D11Buffer* noConstants[]{nullptr};
    context->CSSetConstantBuffers(0, 1, noConstants);
    context->CSSetShader(nullptr, nullptr, 0);
    if (unifiedVisual)
    {
        context->CopyResource(slot.calibrationStaging.Get(), slot.calibration.Get());
    }
    else
    {
        const D3D11_BOX calibrationBox{0, 0, 0, legacyCalibrationBytes, 1, 1};
        context->CopySubresourceRegion(slot.calibrationStaging.Get(), 0, 0, 0, 0, slot.calibration.Get(), 0,
            &calibrationBox);
    }
    const D3D11_BOX metricBox{0, 0, 0, binding.metricCount * sizeof(float), 1, 1};
    context->CopySubresourceRegion(slot.metricsStaging.Get(), 0, 0, 0, 0, slot.metrics.Get(), 0, &metricBox);
    if (remoteVisualLowFps)
    {
        context->CopyResource(slot.remoteVisualLowFpsFreshnessSummaryStaging.Get(),
            slot.remoteVisualLowFpsFreshnessSummary.Get());
    }
    else if (unifiedVisual)
    {
        context->CopyResource(slot.unifiedTileSamplingFailuresStaging.Get(), slot.unifiedTileSamplingFailures.Get());
        context->CopyResource(slot.unifiedFreshnessStaging.Get(), slot.unifiedFreshness.Get());
        context->CopyResource(slot.unifiedPhaseStaging.Get(), slot.unifiedPhase.Get());
    }
    context->End(slot.timestampEnd.Get());
    context->End(slot.timestampDisjoint.Get());
    context->End(slot.completion.Get());

    slot.inputSrv = std::move(inputSrv);
    slot.inputTexture = frame.texture;
    slot.frame = frame;
    slot.frame.texture = nullptr;
    slot.binding = binding;
    slot.remoteVisualLowFpsGeometry = remoteVisualLowFps ? *remoteVisualLowFpsGeometry : pbmodulation::LocalDesktopGeometry{};
    slot.remoteVisualLowFpsPolicy = remoteVisualLowFps ? *remoteVisualLowFpsPolicy : pbmodulation::RemoteVisualLowFpsDecodePolicy{};
    slot.unifiedBootstrap = unifiedVisual ? *unifiedBootstrap : pbmodulation::LocalDesktopObservation{};
    slot.unifiedPolicy = unifiedVisual ? *unifiedPolicy : pbmodulation::UnifiedVisualDecodePolicy{};
    std::copy_n(bootstrapRecord.begin(), slot.bootstrapRecord.size(), slot.bootstrapRecord.begin());
    slot.generation++;
    slot.busy = true;
    slot.cancelled = false;
    slot.unbound = false;
    DemodSubmission submission{slotIndex, slot.generation, frame.metadata.domain, frame.metadata.captureObservation};
    {
        const std::lock_guard lock(state.snapshotMutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.submittedFrames);
        state.snapshot.pendingFrames++;
        state.snapshot.highWater = std::max(state.snapshot.highWater, state.snapshot.pendingFrames);
    }
    output = submission;
    return {};
}
} // namespace

DemodStatus Demodulator::Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
    const std::span<const std::byte> bootstrapRecord, DemodSubmission& output) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    return SubmitInternal(*implementation_, frame, context, bootstrapRecord, nullptr, nullptr, nullptr, nullptr, output);
}

DemodStatus Demodulator::SubmitRemoteVisualLowFps(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
    const std::span<const std::byte> bootstrapRecord, const pbmodulation::LocalDesktopGeometry& geometry,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy, DemodSubmission& output) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    return SubmitInternal(*implementation_, frame, context, bootstrapRecord, std::addressof(geometry),
        std::addressof(policy), nullptr, nullptr, output);
}

DemodStatus Demodulator::SubmitUnifiedVisual(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
    const pbmodulation::LocalDesktopObservation& bootstrap,
    const pbmodulation::UnifiedVisualDecodePolicy& policy, DemodSubmission& output) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    return SubmitInternal(*implementation_, frame, context, bootstrap.canonical44, nullptr, nullptr,
        std::addressof(bootstrap), std::addressof(policy), output);
}

DemodStatus Demodulator::SubmitUnbound(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
    const std::uint64_t expectedVisualProfileId, DemodSubmission& output) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    pbprotocol::BootstrapRecord placeholder;
    placeholder.protocolVersion = pbprotocol::GetProtocolVersion();
    placeholder.sessionTag.value = 1;
    placeholder.visualProfileId = expectedVisualProfileId;
    if (expectedVisualProfileId == pbmodulation::kShapeChromaProfileId)
    {
        placeholder.visualLayoutVersion = pbmodulation::kShapeChromaLayoutVersion;
    }
    else if (expectedVisualProfileId == pbmodulation::kRemoteVisualProfileId)
    {
        placeholder.visualLayoutVersion = pbmodulation::kRemoteVisualLayoutVersion;
    }
    else if (pbmodulation::GetDesktopLevelsProfile(expectedVisualProfileId) != nullptr)
    {
        placeholder.visualLayoutVersion = pbmodulation::kDesktopLevelsLayoutVersion;
    }
    else
    {
        return DemodStatus::Failure(DemodError::UnsupportedProfile, DemodStage::Binding);
    }
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> placeholderBytes{};
    if (!pbprotocol::SerializeBootstrapRecord(placeholder, placeholderBytes))
    {
        return DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding);
    }
    DemodSubmission submission;
    const auto status = Submit(frame, context, placeholderBytes, submission);
    if (!status)
    {
        return status;
    }
    implementation_->slots[submission.slotIndex].unbound = true;
    output = submission;
    return {};
}

namespace
{
DemodPollResult PollInternal(Demodulator::Implementation& state, ID3D11DeviceContext* context, const DemodSubmission& submission,
    const bool unboundCall, const std::span<const std::byte> suppliedBootstrapRecord, DemodFrameResult& output) noexcept
{
    auto status = ValidateOwner(state, context, DemodStage::Completion);
    if (!status)
    {
        return {status, false};
    }
    if (submission.slotIndex >= state.slotCount)
    {
        return {DemodStatus::Failure(DemodError::InvalidSubmission, DemodStage::Completion), false};
    }
    auto& slot = state.slots[submission.slotIndex];
    if (!slot.busy || slot.generation != submission.slotGeneration || slot.frame.metadata.domain != submission.domain ||
        slot.frame.metadata.captureObservation != submission.captureObservation || slot.unbound != unboundCall)
    {
        return {DemodStatus::Failure(DemodError::InvalidSubmission, DemodStage::Completion), false};
    }
    const HRESULT query = context->GetData(slot.completion.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (query == S_FALSE)
    {
        return {{}, false};
    }
    if (FAILED(query))
    {
        const auto failure = ClassifyNativeFailure(state, query, DemodStage::Completion, DemodError::NativeFailure);
        RetireSlot(state, slot, true);
        return {failure, true};
    }
    if (slot.cancelled)
    {
        RetireSlot(state, slot, false);
        const std::lock_guard lock(state.snapshotMutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.cancelledFrames);
        return {DemodStatus::Failure(DemodError::Cancelled, DemodStage::Completion), true};
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timestampDisjoint{};
    std::uint64_t timestampStart = 0;
    std::uint64_t timestampEnd = 0;
    const HRESULT disjointStatus = context->GetData(slot.timestampDisjoint.Get(), &timestampDisjoint,
        sizeof(timestampDisjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT startStatus = context->GetData(slot.timestampStart.Get(), &timestampStart,
        sizeof(timestampStart), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT endStatus = context->GetData(slot.timestampEnd.Get(), &timestampEnd,
        sizeof(timestampEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    for (const HRESULT timingStatus : {disjointStatus, startStatus, endStatus})
    {
        if (FAILED(timingStatus))
        {
            const auto failure = ClassifyNativeFailure(state, timingStatus, DemodStage::Completion, DemodError::NativeFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
    }
    std::uint64_t gpuTime100ns = 0;
    bool gpuTimingValid = disjointStatus == S_OK && startStatus == S_OK && endStatus == S_OK &&
        !timestampDisjoint.Disjoint && timestampDisjoint.Frequency != 0 && timestampEnd >= timestampStart;
    if (gpuTimingValid)
    {
        const std::uint64_t delta = timestampEnd - timestampStart;
        const auto wholeSeconds = pbprotocol::CheckedMultiplyUint64(delta / timestampDisjoint.Frequency, 10000000ULL);
        const auto remainderScaled = pbprotocol::CheckedMultiplyUint64(delta % timestampDisjoint.Frequency, 10000000ULL);
        if (!wholeSeconds || !remainderScaled)
        {
            gpuTimingValid = false;
        }
        else
        {
            const auto converted = pbprotocol::CheckedAddUint64(
                wholeSeconds.Value(), remainderScaled.Value() / timestampDisjoint.Frequency);
            if (!converted)
            {
                gpuTimingValid = false;
            }
            else
            {
                gpuTime100ns = converted.Value();
            }
        }
    }
    Binding evaluationBinding = slot.binding;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> evaluationBootstrap = slot.bootstrapRecord;
    if (unboundCall)
    {
        status = ParseBinding(suppliedBootstrapRecord, evaluationBinding);
        if (!status || evaluationBinding.profileId != slot.binding.profileId || evaluationBinding.mode != slot.binding.mode ||
            evaluationBinding.tilePixels != slot.binding.tilePixels || evaluationBinding.tileCount != slot.binding.tileCount ||
            evaluationBinding.dataBytes != slot.binding.dataBytes || evaluationBinding.metricCount != slot.binding.metricCount ||
            evaluationBinding.codedMetricCount != slot.binding.codedMetricCount)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding,
                status ? 0 : status.nativeError), true};
        }
        std::copy(suppliedBootstrapRecord.begin(), suppliedBootstrapRecord.end(), evaluationBootstrap.begin());
    }

    const bool remoteVisualLowFps = slot.binding.mode == ProfileMode::RemoteVisualLowFps;
    const bool unifiedVisual = slot.binding.mode == ProfileMode::UnifiedVisual;
    const std::size_t calibrationReadbackBytes = unifiedVisual ? calibrationBytes : legacyCalibrationBytes;
    std::array<std::array<float, 4>, calibrationEntries> calibration{};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT native = context->Map(slot.calibrationStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(native))
    {
        const auto failure = ClassifyNativeFailure(state, native, DemodStage::Readback, DemodError::MapFailure);
        RetireSlot(state, slot, true);
        return {failure, true};
    }
    if (mapped.pData == nullptr)
    {
        context->Unmap(slot.calibrationStaging.Get(), 0);
        const auto failure = ClassifyNativeFailure(state, E_FAIL, DemodStage::Readback, DemodError::MapFailure);
        RetireSlot(state, slot, true);
        return {failure, true};
    }
    std::memcpy(calibration.data(), mapped.pData, calibrationReadbackBytes);
    context->Unmap(slot.calibrationStaging.Get(), 0);
    pbmodulation::UnifiedBaseLumaObservation unifiedBaseLuma;
    pbmodulation::UnifiedFineLumaObservation unifiedFineLuma;
    pbmodulation::UnifiedChromaObservation unifiedChroma;
    status = unifiedVisual ? ResolveUnifiedCalibration(calibration, slot.unifiedPolicy,
        unifiedBaseLuma, unifiedFineLuma, unifiedChroma) : ValidateCalibration(slot.binding.mode, calibration,
            remoteVisualLowFps ? std::addressof(slot.remoteVisualLowFpsPolicy) : nullptr);
    if (!status)
    {
        RetireSlot(state, slot, true);
        return {status, true};
    }
    mapped = {};
    native = context->Map(slot.metricsStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(native))
    {
        const auto failure = ClassifyNativeFailure(state, native, DemodStage::Readback, DemodError::MapFailure);
        RetireSlot(state, slot, true);
        return {failure, true};
    }
    if (mapped.pData == nullptr)
    {
        context->Unmap(slot.metricsStaging.Get(), 0);
        const auto failure = ClassifyNativeFailure(state, E_FAIL, DemodStage::Readback, DemodError::MapFailure);
        RetireSlot(state, slot, true);
        return {failure, true};
    }
    const std::size_t metricBytes = static_cast<std::size_t>(slot.binding.metricCount) * sizeof(float);
    std::memcpy(state.cpuMetrics.data(), mapped.pData, metricBytes);
    context->Unmap(slot.metricsStaging.Get(), 0);
    auto metrics = std::span(state.cpuMetrics).first(slot.binding.metricCount);
    if (!std::ranges::all_of(metrics, [](const float value) { return std::isfinite(value); }))
    {
        RetireSlot(state, slot, true);
        return {DemodStatus::Failure(DemodError::NonFiniteMetric, DemodStage::Readback), true};
    }
    if (unifiedVisual)
    {
        std::array<std::array<float, 4>, pbmodulation::kUnifiedFreshnessRegionCount> freshnessValues{};
        mapped = {};
        native = context->Map(slot.unifiedFreshnessStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(native) || mapped.pData == nullptr)
        {
            if (SUCCEEDED(native))
            {
                context->Unmap(slot.unifiedFreshnessStaging.Get(), 0);
            }
            const auto failure = ClassifyNativeFailure(state, FAILED(native) ? native : E_FAIL,
                DemodStage::Readback, DemodError::MapFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
        std::memcpy(freshnessValues.data(), mapped.pData, unifiedFreshnessBytes);
        context->Unmap(slot.unifiedFreshnessStaging.Get(), 0);

        std::array<std::array<float, 4>, unifiedPhaseEntries> phaseValues{};
        mapped = {};
        native = context->Map(slot.unifiedPhaseStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(native) || mapped.pData == nullptr)
        {
            if (SUCCEEDED(native))
            {
                context->Unmap(slot.unifiedPhaseStaging.Get(), 0);
            }
            const auto failure = ClassifyNativeFailure(state, FAILED(native) ? native : E_FAIL,
                DemodStage::Readback, DemodError::MapFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
        std::memcpy(phaseValues.data(), mapped.pData, unifiedPhaseBytes);
        context->Unmap(slot.unifiedPhaseStaging.Get(), 0);
        status = ApplyUnifiedPhaseObservations(phaseValues, evaluationBinding.frameSequence,
            slot.unifiedPolicy, calibration, unifiedBaseLuma, unifiedFineLuma);
        if (!status)
        {
            RetireSlot(state, slot, true);
            return {status, true};
        }

        mapped = {};
        native = context->Map(slot.unifiedTileSamplingFailuresStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(native) || mapped.pData == nullptr)
        {
            if (SUCCEEDED(native))
            {
                context->Unmap(slot.unifiedTileSamplingFailuresStaging.Get(), 0);
            }
            const auto failure = ClassifyNativeFailure(state, FAILED(native) ? native : E_FAIL,
                DemodStage::Readback, DemodError::MapFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
        const auto* const tileSamplingValues = static_cast<const std::uint32_t*>(mapped.pData);
        bool tileSamplingValid = true;
        for (std::size_t tile = 0; tile < state.unifiedTileSamplingFailures.size(); tile++)
        {
            tileSamplingValid = tileSamplingValid && tileSamplingValues[tile] <= 1;
            state.unifiedTileSamplingFailures[tile] = static_cast<std::uint8_t>(tileSamplingValues[tile]);
        }
        context->Unmap(slot.unifiedTileSamplingFailuresStaging.Get(), 0);
        if (!tileSamplingValid)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Readback), true};
        }

        std::array<pbmodulation::UnifiedFreshnessObservation, pbmodulation::kUnifiedFreshnessRegionCount>
            freshness{};
        for (std::size_t region = 0; region < freshness.size(); region++)
        {
            const auto& value = freshnessValues[region];
            if (!std::ranges::all_of(value, [](const float component) { return std::isfinite(component); }) ||
                value[0] < 0 || value[0] > pbmodulation::kLocalDesktopTimingBits ||
                value[1] < 0 || value[2] < 0 || value[2] > 1)
            {
                RetireSlot(state, slot, true);
                return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Readback), true};
            }
            const bool samplesValid = value[2] == 1.0f;
            freshness[region].bitErrors = samplesValid ? static_cast<std::uint16_t>(std::lround(value[0])) :
                static_cast<std::uint16_t>(pbmodulation::kLocalDesktopTimingBits);
            freshness[region].residual = samplesValid ? value[1] : 1.0;
            freshness[region].current = samplesValid &&
                static_cast<double>(freshness[region].bitErrors) / pbmodulation::kLocalDesktopTimingBits <=
                    slot.unifiedPolicy.maximumTimingBitErrorFraction &&
                freshness[region].residual <= slot.unifiedPolicy.locator.maximumTimingResidual;
        }
        const auto metadata = slot.frame.metadata;
        const auto unifiedBootstrap = slot.unifiedBootstrap;
        const auto unifiedPolicy = slot.unifiedPolicy;
        pbmodulation::UnifiedPreparedMetricFrame prepared;
        prepared.bootstrap = unifiedBootstrap;
        prepared.logicalMetrics = metrics;
        prepared.tileSamplingFailures = state.unifiedTileSamplingFailures;
        prepared.freshness = freshness;
        prepared.baseLuma = unifiedBaseLuma;
        prepared.fineLuma = unifiedFineLuma;
        prepared.chroma = unifiedChroma;
        auto unifiedObservation = state.unifiedOracle.DecodePreparedMixedFrame(prepared, {}, unifiedPolicy);
        if (!unifiedObservation.inputValid)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Fec), true};
        }
        const auto acceptedUnified = state.unifiedOracle.GetAcceptedBlocks();
        const auto evaluationMetrics = metrics.first(evaluationBinding.codedMetricCount);
        std::uint32_t zeroMagnitudeMetrics = 0;
        double minimumAbsoluteMetric = (std::numeric_limits<double>::max)();
        double absoluteMetricSum = 0;
        for (const float metric : evaluationMetrics)
        {
            const double magnitude = std::abs(static_cast<double>(metric));
            zeroMagnitudeMetrics += static_cast<std::uint32_t>(magnitude == 0);
            minimumAbsoluteMetric = std::min(minimumAbsoluteMetric, magnitude);
            absoluteMetricSum += magnitude;
        }
        RetireSlot(state, slot, false);

        DemodFrameResult result;
        result.metadata = metadata;
        result.visualProfileId = evaluationBinding.profileId;
        result.metricReadbackBytes = metricBytes + calibrationBytes + unifiedTileSamplingBytes +
            unifiedFreshnessBytes + unifiedPhaseBytes;
        result.gpuTime100ns = gpuTime100ns;
        result.gpuTimingValid = gpuTimingValid;
        result.remoteMetricSummaryAvailable = true;
        result.remoteMetricSamples = evaluationBinding.codedMetricCount;
        result.remoteZeroMagnitudeMetrics = zeroMagnitudeMetrics;
        result.remoteMinimumAbsoluteMetric = evaluationMetrics.empty() ? 0 : minimumAbsoluteMetric;
        result.remoteMeanAbsoluteMetric = evaluationMetrics.empty() ? 0 :
            absoluteMetricSum / static_cast<double>(evaluationMetrics.size());
        result.remoteFreshnessRegions = pbmodulation::kUnifiedFreshnessRegionCount;
        result.remoteStaleRegions = static_cast<std::uint32_t>(std::ranges::count_if(freshness,
            [](const pbmodulation::UnifiedFreshnessObservation& value) { return !value.current; }));
        result.remoteFreshnessErasedDataMetrics = static_cast<std::uint32_t>(std::ranges::count_if(
            state.unifiedOracle.GetSoftMetrics(), [](const pbmodulation::UnifiedSoftMetric& metric)
            {
                return metric.erasureReason == pbmodulation::UnifiedErasureReason::LocalStaleRegion;
            }));
        result.remoteUnreliableSymbols = static_cast<std::uint32_t>(std::ranges::count(
            state.unifiedTileSamplingFailures, std::uint8_t{1}));
        result.unifiedObservation = std::move(unifiedObservation);
        result.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(acceptedUnified.size());
        std::copy(acceptedUnified.begin(), acceptedUnified.end(), result.acceptedUnifiedBlocks.begin());
        {
            const std::lock_guard lock(state.snapshotMutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
            state.snapshot.metricReadbackBytes = pbprotocol::SaturatingAddUnsigned(
                state.snapshot.metricReadbackBytes, result.metricReadbackBytes);
            if (result.gpuTimingValid)
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.gpuTimingSamples);
                state.snapshot.gpuTimeTotal100ns = pbprotocol::SaturatingAddUnsigned(
                    state.snapshot.gpuTimeTotal100ns, result.gpuTime100ns);
                state.snapshot.gpuTimeHighWater100ns = std::max(
                    state.snapshot.gpuTimeHighWater100ns, result.gpuTime100ns);
            }
            else
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.gpuTimingUnavailable);
            }
        }
        output = std::move(result);
        return {{}, true};
    }
    pbmodulation::RemoteVisualMetricResolution remoteResolution;
    pbmodulation::RemoteVisualLowFpsMetricResolution remoteVisualLowFpsResolution;
    std::uint32_t remoteUnreliableSymbols = 0;
    if (slot.binding.mode == ProfileMode::RemoteVisual)
    {
        remoteResolution = pbmodulation::ResolveRemoteVisualPhysicalMetrics(metrics,
            evaluationBinding.sessionTag, evaluationBinding.frameSequence,
            std::span(state.logicalMetrics).first(evaluationBinding.codedMetricCount));
        if (!remoteResolution.valid)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding), true};
        }
        metrics = std::span(state.logicalMetrics).first(evaluationBinding.codedMetricCount);
    }
    else if (remoteVisualLowFps)
    {
        std::array<RemoteVisualLowFpsFreshnessSummary, remoteVisualLowFpsFreshnessSummaryEntries> summaries{};
        mapped = {};
        native = context->Map(slot.remoteVisualLowFpsFreshnessSummaryStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(native))
        {
            const auto failure = ClassifyNativeFailure(state, native, DemodStage::Readback, DemodError::MapFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
        if (mapped.pData == nullptr)
        {
            context->Unmap(slot.remoteVisualLowFpsFreshnessSummaryStaging.Get(), 0);
            const auto failure = ClassifyNativeFailure(state, E_FAIL, DemodStage::Readback, DemodError::MapFailure);
            RetireSlot(state, slot, true);
            return {failure, true};
        }
        std::memcpy(summaries.data(), mapped.pData, remoteVisualLowFpsFreshnessSummaryBytes);
        context->Unmap(slot.remoteVisualLowFpsFreshnessSummaryStaging.Get(), 0);
        const auto& globalSummary = summaries[pbmodulation::kRemoteVisualFreshnessRegionCount];
        if (globalSummary.mismatches != 0 || globalSummary.stale != 0 || globalSummary.erasedDataMetrics != 0)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidFrame, DemodStage::Readback,
                static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::PixelReadFailure)), true};
        }
        remoteUnreliableSymbols = globalSummary.erasures;
        remoteVisualLowFpsResolution.freshnessRegions = pbmodulation::kRemoteVisualEligibleFreshnessRegions;
        std::uint64_t freshnessTagMismatches = 0;
        std::uint64_t freshnessTagErasures = 0;
        std::uint64_t erasedDataMetrics = 0;
        for (std::uint32_t region = 0; region < pbmodulation::kRemoteVisualFreshnessRegionCount; region++)
        {
            const auto& summary = summaries[region];
            if (summary.stale > 1)
            {
                RetireSlot(state, slot, true);
                return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Readback), true};
            }
            freshnessTagMismatches += summary.mismatches;
            freshnessTagErasures += summary.erasures;
            erasedDataMetrics += summary.erasedDataMetrics;
            remoteVisualLowFpsResolution.staleRegions += summary.stale;
        }
        if (freshnessTagMismatches > pbmodulation::kRemoteVisualTileCount ||
            freshnessTagErasures > pbmodulation::kRemoteVisualTileCount ||
            erasedDataMetrics > pbmodulation::kRemoteVisualLowFpsCodedBits ||
            remoteVisualLowFpsResolution.staleRegions > pbmodulation::kRemoteVisualEligibleFreshnessRegions)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Readback), true};
        }
        remoteVisualLowFpsResolution.freshnessTagMismatches = static_cast<std::uint32_t>(freshnessTagMismatches);
        remoteVisualLowFpsResolution.freshnessTagErasures = static_cast<std::uint32_t>(freshnessTagErasures);
        remoteVisualLowFpsResolution.erasedDataMetrics = static_cast<std::uint32_t>(erasedDataMetrics);
        remoteVisualLowFpsResolution.valid = true;
        for (float& metric : metrics)
        {
            float calibratedMetric = 0;
            if (!pbmodulation::CalibrateRemoteVisualLowFpsMetric(metric, calibratedMetric))
            {
                RetireSlot(state, slot, true);
                return {DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration), true};
            }
            metric = calibratedMetric;
        }
    }
    else if (unboundCall)
    {
        const std::uint32_t metricsPerTile = slot.binding.metricCount / slot.binding.tileCount;
        const auto ToLogical = [&](const std::uint32_t physical, const std::uint64_t sequence)
        {
            const auto* const permutation = pbinterleave::GetDesktopLevelsPermutation(slot.binding.tilePixels);
            return permutation == nullptr ? slot.binding.tileCount : permutation->ToLogical(physical, sequence);
        };
        if (metricsPerTile == 0 || metricsPerTile * slot.binding.tileCount != slot.binding.metricCount ||
            ToLogical(0, 0) >= slot.binding.tileCount)
        {
            RetireSlot(state, slot, true);
            return {DemodStatus::Failure(DemodError::InvalidBinding, DemodStage::Binding), true};
        }
        for (std::uint32_t physical = 0; physical < slot.binding.tileCount; physical++)
        {
            const std::uint32_t phaseZeroLogical = ToLogical(physical, 0);
            const std::uint32_t actualLogical = ToLogical(physical, evaluationBinding.interleavePhase);
            for (std::uint32_t metric = 0; metric < metricsPerTile; metric++)
            {
                state.logicalMetrics[static_cast<std::size_t>(actualLogical) * metricsPerTile + metric] =
                    metrics[static_cast<std::size_t>(phaseZeroLogical) * metricsPerTile + metric];
            }
        }
        metrics = std::span(state.logicalMetrics).first(slot.binding.metricCount);
    }
    std::fill_n(state.hard.begin(), slot.binding.dataBytes, std::byte{0});
    const std::size_t firstHardBit = state.evaluationMode == pbdesktoplevels::EvaluationMode::Transport ?
        static_cast<std::size_t>(slot.binding.codewords) * pbdesktoplevels::kCodewordBits : 0;
    const auto evaluationMetrics = metrics.first(evaluationBinding.codedMetricCount);
    const bool remoteMetricSummaryAvailable = slot.binding.mode == ProfileMode::RemoteVisual || remoteVisualLowFps;
    std::uint32_t remoteZeroMagnitudeMetrics = 0;
    double remoteMinimumAbsoluteMetric = (std::numeric_limits<double>::max)();
    double remoteAbsoluteMetricSum = 0;
    if (remoteMetricSummaryAvailable)
    {
        for (const float metric : evaluationMetrics)
        {
            const double magnitude = std::abs(static_cast<double>(metric));
            if (magnitude == 0)
            {
                remoteZeroMagnitudeMetrics++;
            }
            remoteMinimumAbsoluteMetric = std::min(remoteMinimumAbsoluteMetric, magnitude);
            remoteAbsoluteMetricSum += magnitude;
        }
    }
    for (std::size_t bit = firstHardBit; bit < evaluationMetrics.size(); bit++)
    {
        if (metrics[bit] < 0)
        {
            state.hard[bit / 8] |= static_cast<std::byte>(1u << (bit % 8));
        }
    }
    const auto metadata = slot.frame.metadata;
    RetireSlot(state, slot, false);

    DemodFrameResult result;
    result.metadata = metadata;
    result.visualProfileId = evaluationBinding.profileId;
    result.metricReadbackBytes = metricBytes + legacyCalibrationBytes +
        (remoteVisualLowFps ? remoteVisualLowFpsFreshnessSummaryBytes : 0);
    result.gpuTime100ns = gpuTime100ns;
    result.gpuTimingValid = gpuTimingValid;
    result.remoteMetricSummaryAvailable = remoteMetricSummaryAvailable;
    result.remoteMetricSamples = remoteMetricSummaryAvailable ? evaluationBinding.codedMetricCount : 0;
    result.remoteZeroMagnitudeMetrics = remoteZeroMagnitudeMetrics;
    result.remoteMinimumAbsoluteMetric = remoteMetricSummaryAvailable && !evaluationMetrics.empty() ?
        remoteMinimumAbsoluteMetric : 0;
    result.remoteMeanAbsoluteMetric = remoteMetricSummaryAvailable && !evaluationMetrics.empty() ?
        remoteAbsoluteMetricSum / static_cast<double>(evaluationMetrics.size()) : 0;
    result.remoteFreshnessRegions = remoteVisualLowFps ? remoteVisualLowFpsResolution.freshnessRegions : remoteResolution.freshnessRegions;
    result.remoteStaleRegions = remoteVisualLowFps ? remoteVisualLowFpsResolution.staleRegions : remoteResolution.staleRegions;
    result.remoteFreshnessTagMismatches = remoteVisualLowFps ?
        remoteVisualLowFpsResolution.freshnessTagMismatches : remoteResolution.freshnessTagMismatches;
    result.remoteFreshnessTagErasures = remoteVisualLowFps ?
        remoteVisualLowFpsResolution.freshnessTagErasures : remoteResolution.freshnessTagErasures;
    result.remoteFreshnessErasedDataMetrics = remoteVisualLowFps ?
        remoteVisualLowFpsResolution.erasedDataMetrics : remoteResolution.erasedDataMetrics;
    result.remoteUnreliableSymbols = remoteUnreliableSymbols;
    result.evaluation = state.evaluator.EvaluateCodewords(evaluationBootstrap,
        std::span(state.hard).first(evaluationBinding.dataBytes), evaluationMetrics, state.evaluationMode);
    const auto accepted = state.evaluator.GetAcceptedTransportBlocks();
    result.acceptedTransportBlockCount = static_cast<std::uint32_t>(accepted.size());
    std::copy(accepted.begin(), accepted.end(), result.acceptedTransportBlocks.begin());
    const auto acceptedControl = state.evaluator.GetAcceptedRemoteControlBlocks();
    result.acceptedRemoteControlBlockCount = static_cast<std::uint32_t>(acceptedControl.size());
    std::copy(acceptedControl.begin(), acceptedControl.end(), result.acceptedRemoteControlBlocks.begin());
    {
        const std::lock_guard lock(state.snapshotMutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
        state.snapshot.metricReadbackBytes = pbprotocol::SaturatingAddUnsigned(state.snapshot.metricReadbackBytes, result.metricReadbackBytes);
        if (result.gpuTimingValid)
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.gpuTimingSamples);
            state.snapshot.gpuTimeTotal100ns = pbprotocol::SaturatingAddUnsigned(state.snapshot.gpuTimeTotal100ns, result.gpuTime100ns);
            state.snapshot.gpuTimeHighWater100ns = std::max(state.snapshot.gpuTimeHighWater100ns, result.gpuTime100ns);
        }
        else
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.gpuTimingUnavailable);
        }
    }
    output = result;
    return {{}, true};
}
} // namespace

DemodPollResult Demodulator::Poll(ID3D11DeviceContext* context, const DemodSubmission& submission, DemodFrameResult& output) noexcept
{
    if (!implementation_)
    {
        return {DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion), false};
    }
    return PollInternal(*implementation_, context, submission, false, {}, output);
}

DemodPollResult Demodulator::PollUnbound(ID3D11DeviceContext* context, const DemodSubmission& submission,
    const std::span<const std::byte> bootstrapRecord, DemodFrameResult& output) noexcept
{
    if (!implementation_)
    {
        return {DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion), false};
    }
    return PollInternal(*implementation_, context, submission, true, bootstrapRecord, output);
}

DemodStatus Demodulator::RetireAfterExternalCompletion(const DemodSubmission& submission) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion);
    }
    auto& state = *implementation_;
    if (submission.slotIndex >= state.slotCount)
    {
        return DemodStatus::Failure(DemodError::InvalidSubmission, DemodStage::Completion);
    }
    auto& slot = state.slots[submission.slotIndex];
    if (!slot.busy || slot.generation != submission.slotGeneration || slot.frame.metadata.domain != submission.domain ||
        slot.frame.metadata.captureObservation != submission.captureObservation)
    {
        return DemodStatus::Failure(DemodError::InvalidSubmission, DemodStage::Completion);
    }
    RetireSlot(state, slot, false);
    const std::lock_guard lock(state.snapshotMutex);
    pbprotocol::SaturatingIncrementUnsigned(state.snapshot.cancelledFrames);
    return {};
}

DemodStatus Demodulator::InvalidateDomain(const ScreenCaptureDomain& domain) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    auto& state = *implementation_;
    if (GetCurrentThreadId() != state.ownerThread)
    {
        return DemodStatus::Failure(DemodError::WrongThread, DemodStage::Submission);
    }
    if (state.activeDomain && *state.activeDomain == domain)
    {
        state.activeDomain.reset();
    }
    for (std::uint32_t index = 0; index < state.slotCount; index++)
    {
        auto& slot = state.slots[index];
        if (slot.busy && slot.frame.metadata.domain == domain)
        {
            slot.cancelled = true;
        }
    }
    return {};
}

DemodStatus Demodulator::ShutdownAfterExternalCompletion() noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Shutdown);
    }
    auto& state = *implementation_;
    {
        const std::lock_guard lock(state.snapshotMutex);
        if (state.snapshot.pendingFrames != 0)
        {
            return DemodStatus::Failure(DemodError::ShutdownRequired, DemodStage::Shutdown);
        }
        state.snapshot.shutdown = true;
    }
    state.activeDomain.reset();
    return {};
}

DemodStatus Demodulator::Shutdown(ID3D11DeviceContext* context) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Shutdown);
    }
    auto& state = *implementation_;
    const auto owner = ValidateOwner(state, context, DemodStage::Shutdown);
    if (!owner)
    {
        return owner;
    }
    {
        const std::lock_guard lock(state.snapshotMutex);
        if (state.snapshot.pendingFrames != 0)
        {
            return DemodStatus::Failure(DemodError::ShutdownRequired, DemodStage::Shutdown);
        }
        state.snapshot.shutdown = true;
    }
    state.activeDomain.reset();
    return {};
}

DemodSnapshot Demodulator::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    const std::lock_guard lock(implementation_->snapshotMutex);
    return implementation_->snapshot;
}

const char* GetDemodErrorName(const DemodError error) noexcept
{
    switch (error)
    {
    case DemodError::None: return "None";
    case DemodError::InvalidConfiguration: return "InvalidConfiguration";
    case DemodError::WrongThread: return "WrongThread";
    case DemodError::WrongDevice: return "WrongDevice";
    case DemodError::AdapterMismatch: return "AdapterMismatch";
    case DemodError::InvalidFrame: return "InvalidFrame";
    case DemodError::InvalidBinding: return "InvalidBinding";
    case DemodError::UnsupportedProfile: return "UnsupportedProfile";
    case DemodError::ResourceLimit: return "ResourceLimit";
    case DemodError::Busy: return "Busy";
    case DemodError::ShaderCompileFailure: return "ShaderCompileFailure";
    case DemodError::NativeFailure: return "NativeFailure";
    case DemodError::DeviceLost: return "DeviceLost";
    case DemodError::InvalidSubmission: return "InvalidSubmission";
    case DemodError::MapFailure: return "MapFailure";
    case DemodError::NonFiniteMetric: return "NonFiniteMetric";
    case DemodError::CalibrationFailure: return "CalibrationFailure";
    case DemodError::Cancelled: return "Cancelled";
    case DemodError::ShutdownRequired: return "ShutdownRequired";
    }
    return "Unknown";
}

} // namespace pbdemodd3d11
