#include "pbdemodd3d11/demodulator.h"

#include "demod_shader_source.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/shape_chroma.h"
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
inline constexpr std::uint32_t calibrationEntries = calibrationPilotCount * calibrationStateCount;
inline constexpr std::uint32_t calibrationBytes = calibrationEntries * sizeof(float) * 4;

enum class ProfileMode : std::uint32_t
{
    DesktopLevels2 = 1, DesktopLevels4 = 2, ShapeChroma = 3, RemoteVisual = 4
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
};
static_assert(sizeof(FrameConstants) == 32);

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

DemodStatus CompileShader(ID3D11Device* device, const char* entryPoint, ComPtr<ID3D11ComputeShader>& output) noexcept
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT compile = D3DCompile(detail::kDemodComputeShader, sizeof(detail::kDemodComputeShader) - 1,
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
    const std::array<std::array<float, 4>, calibrationEntries>& calibration) noexcept
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
    if (mode == ProfileMode::RemoteVisual)
    {
        std::array<double, 2> centroids{};
        for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
        {
            for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
            {
                const std::size_t level = endpoint == 0 ? 0 : 3;
                const auto& entry = calibration[pilot * calibrationStateCount + level];
                if (entry[1] > 576 || entry[2] != 0 || entry[3] != 0)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
                centroids[endpoint] += entry[0] / calibrationPilotCount;
            }
        }
        for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
        {
            const std::size_t level = endpoint == 0 ? 0 : 3;
            for (std::size_t pilot = 0; pilot < calibrationPilotCount; pilot++)
            {
                if (std::abs(static_cast<double>(calibration[pilot * calibrationStateCount + level][0]) - centroids[endpoint]) > 24)
                {
                    return DemodStatus::Failure(DemodError::CalibrationFailure, DemodStage::Calibration);
                }
            }
        }
        return centroids[1] - centroids[0] < 96 ?
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
        ComPtr<ID3D11Query> completion;
        ComPtr<ID3D11Query> timestampDisjoint;
        ComPtr<ID3D11Query> timestampStart;
        ComPtr<ID3D11Query> timestampEnd;
        ComPtr<ID3D11Texture2D> inputTexture;
        ComPtr<ID3D11ShaderResourceView> inputSrv;
        ScreenCaptureFrame frame;
        Binding binding;
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
    std::array<Slot, maximumSlots> slots;
    std::uint32_t slotCount = 0;
    DWORD ownerThread = 0;
    std::optional<ScreenCaptureDomain> activeDomain;
    std::array<float, maximumMetricCount> cpuMetrics{};
    std::array<float, maximumMetricCount> logicalMetrics{};
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> hard{};
    pbdesktoplevels::ReferenceChannel evaluator;
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

DemodStatus ValidateFrame(Demodulator::Implementation& state, const ScreenCaptureFrame& frame) noexcept
{
    const auto& metadata = frame.metadata;
    const auto physicalWidth = static_cast<std::int64_t>(metadata.physicalRoi.right) - metadata.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(metadata.physicalRoi.bottom) - metadata.physicalRoi.top;
    const bool cursorProvenAbsent = metadata.sourceCursorState == pbcapturenormalize::CursorState::Excluded ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::SeparatePointer ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::KnownAbsent;
    if (frame.texture == nullptr || metadata.domain.captureEpoch == 0 || !NonzeroSourceId(metadata.domain) || metadata.captureObservation == 0 ||
        frame.metadata.sourceGeneration == 0 || frame.metadata.slotGeneration == 0 || frame.metadata.roiSize.width != 1920 ||
        frame.metadata.roiSize.height != 1080 || frame.metadata.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
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
    if (!EqualLuid(frame.metadata.adapterLuid, state.snapshot.adapterLuid))
    {
        return DemodStatus::Failure(DemodError::AdapterMismatch, DemodStage::Submission);
    }
    D3D11_TEXTURE2D_DESC description{};
    frame.texture->GetDesc(&description);
    if (description.Width != 1920 || description.Height != 1080 || description.MipLevels != 1 || description.ArraySize != 1 ||
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
    if (!metricPairBytes || !calibrationPairBytes)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Configuration);
    }
    const auto perSlotFirst = pbprotocol::CheckedAddUint64(metricPairBytes.Value(), calibrationPairBytes.Value());
    const auto perSlotBytes = perSlotFirst ? pbprotocol::CheckedAddUint64(perSlotFirst.Value(), sizeof(FrameConstants)) : perSlotFirst;
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
    const auto residentBytesResult = residentFourth ? pbprotocol::CheckedAddUint64(residentFourth.Value(), 1024ULL * 1024) : residentFourth;
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
        state->evaluationMode = config.evaluationMode;
        DemodStatus status = GetAdapterLuid(device, state->snapshot.adapterLuid);
        if (!status)
        {
            return status;
        }
        const std::array<std::pair<const char*, ComPtr<ID3D11ComputeShader>*>, 5> shaders{{
            {"CalibrateChromaCS", std::addressof(state->calibrateChroma)},
            {"CalibrateLevelsCS", std::addressof(state->calibrateLevels)},
            {"DemodShapeChromaCS", std::addressof(state->demodShapeChroma)},
            {"DemodDesktopLevelsCS", std::addressof(state->demodDesktopLevels)},
            {"DemodRemoteVisualCS", std::addressof(state->demodRemoteVisual)}}};
        for (const auto& entry : shaders)
        {
            status = CompileShader(device, entry.first, *entry.second);
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

DemodStatus Demodulator::Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
    const std::span<const std::byte> bootstrapRecord, DemodSubmission& output) noexcept
{
    if (!implementation_)
    {
        return DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    auto& state = *implementation_;
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
    status = ValidateFrame(state, frame);
    if (!status)
    {
        return status;
    }
    Binding binding;
    status = ParseBinding(bootstrapRecord, binding);
    if (!status)
    {
        return status;
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
    ComPtr<ID3D11ShaderResourceView> inputSrv;
    const HRESULT createSrv = state.device->CreateShaderResourceView(frame.texture, nullptr, &inputSrv);
    if (FAILED(createSrv))
    {
        return ClassifyNativeFailure(state, createSrv, DemodStage::Resource, DemodError::NativeFailure);
    }
    const FrameConstants constants{static_cast<std::uint32_t>(binding.mode), binding.tilePixels, binding.tileCount,
        binding.rowTiles, binding.interleavePhase, binding.metricCount, binding.codedMetricCount, 0};
    context->Begin(slot.timestampDisjoint.Get());
    context->End(slot.timestampStart.Get());
    context->UpdateSubresource(slot.constants.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* constantBuffers[]{slot.constants.Get()};
    context->CSSetConstantBuffers(0, 1, constantBuffers);
    ID3D11ShaderResourceView* calibrationInputs[]{inputSrv.Get(), nullptr};
    context->CSSetShaderResources(0, 2, calibrationInputs);
    ID3D11UnorderedAccessView* calibrationOutputs[]{slot.calibrationUav.Get(), nullptr};
    context->CSSetUnorderedAccessViews(0, 2, calibrationOutputs, nullptr);
    context->CSSetShader(binding.mode == ProfileMode::ShapeChroma ? state.calibrateChroma.Get() : state.calibrateLevels.Get(), nullptr, 0);
    context->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* noOutputs[]{nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
    ID3D11ShaderResourceView* demodInputs[]{inputSrv.Get(), slot.calibrationSrv.Get()};
    context->CSSetShaderResources(0, 2, demodInputs);
    ID3D11UnorderedAccessView* demodOutputs[]{nullptr, slot.metricsUav.Get()};
    context->CSSetUnorderedAccessViews(0, 2, demodOutputs, nullptr);
    ID3D11ComputeShader* const demodShader = binding.mode == ProfileMode::ShapeChroma ? state.demodShapeChroma.Get() :
        binding.mode == ProfileMode::RemoteVisual ? state.demodRemoteVisual.Get() : state.demodDesktopLevels.Get();
    context->CSSetShader(demodShader, nullptr, 0);
    context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
    context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
    ID3D11ShaderResourceView* noInputs[]{nullptr, nullptr};
    context->CSSetShaderResources(0, 2, noInputs);
    ID3D11Buffer* noConstants[]{nullptr};
    context->CSSetConstantBuffers(0, 1, noConstants);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(slot.calibrationStaging.Get(), slot.calibration.Get());
    context->CopyResource(slot.metricsStaging.Get(), slot.metrics.Get());
    context->End(slot.timestampEnd.Get());
    context->End(slot.timestampDisjoint.Get());
    context->End(slot.completion.Get());

    slot.inputSrv = std::move(inputSrv);
    slot.inputTexture = frame.texture;
    slot.frame = frame;
    slot.frame.texture = nullptr;
    slot.binding = binding;
    std::copy(bootstrapRecord.begin(), bootstrapRecord.end(), slot.bootstrapRecord.begin());
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
    std::memcpy(calibration.data(), mapped.pData, calibrationBytes);
    context->Unmap(slot.calibrationStaging.Get(), 0);
    status = ValidateCalibration(slot.binding.mode, calibration);
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
    pbmodulation::RemoteVisualMetricResolution remoteResolution;
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
    const bool remoteMetricSummaryAvailable = slot.binding.mode == ProfileMode::RemoteVisual;
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
    result.metricReadbackBytes = metricBytes + calibrationBytes;
    result.gpuTime100ns = gpuTime100ns;
    result.gpuTimingValid = gpuTimingValid;
    result.remoteMetricSummaryAvailable = remoteMetricSummaryAvailable;
    result.remoteMetricSamples = remoteMetricSummaryAvailable ? evaluationBinding.codedMetricCount : 0;
    result.remoteZeroMagnitudeMetrics = remoteZeroMagnitudeMetrics;
    result.remoteMinimumAbsoluteMetric = remoteMetricSummaryAvailable && !evaluationMetrics.empty() ?
        remoteMinimumAbsoluteMetric : 0;
    result.remoteMeanAbsoluteMetric = remoteMetricSummaryAvailable && !evaluationMetrics.empty() ?
        remoteAbsoluteMetricSum / static_cast<double>(evaluationMetrics.size()) : 0;
    result.remoteFreshnessRegions = remoteResolution.freshnessRegions;
    result.remoteStaleRegions = remoteResolution.staleRegions;
    result.remoteFreshnessTagMismatches = remoteResolution.freshnessTagMismatches;
    result.remoteFreshnessTagErasures = remoteResolution.freshnessTagErasures;
    result.remoteFreshnessErasedDataMetrics = remoteResolution.erasedDataMetrics;
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
