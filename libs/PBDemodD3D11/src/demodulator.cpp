#include "pbdemodd3d11/demodulator.h"

#include "demod_shader_source.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/shape_chroma.h"
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
    DesktopLevels2 = 1, DesktopLevels4 = 2, ShapeChroma = 3
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
    std::uint32_t codewords = 0;
    std::uint32_t paddingBytes = 0;
    std::uint32_t interleavePhase = 0;
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
    if (binding.profileId == pbmodulation::kShapeChromaProfileId &&
        parsed.Value().visualLayoutVersion == pbmodulation::kShapeChromaLayoutVersion)
    {
        binding.mode = ProfileMode::ShapeChroma;
        binding.tilePixels = pbmodulation::kShapeChromaTilePixels;
        binding.tileCount = pbmodulation::kShapeChromaTileCount;
        binding.rowTiles = 432;
        binding.dataBytes = pbmodulation::kShapeChromaDataBytes;
        binding.metricCount = static_cast<std::uint32_t>(pbmodulation::kShapeChromaMaximumBits);
        binding.codewords = pbmodulation::kShapeChromaCodewords;
        binding.paddingBytes = pbmodulation::kShapeChromaPaddingBytes;
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
        binding.codewords = profile->codewords;
        binding.paddingBytes = profile->paddingBytes;
    }
    if (binding.metricCount > maximumMetricCount || binding.codewords > pbdesktoplevels::kMaximumCodewords ||
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
        ComPtr<ID3D11Texture2D> inputTexture;
        ComPtr<ID3D11ShaderResourceView> inputSrv;
        ScreenCaptureFrame frame;
        Binding binding;
        std::array<std::byte, 44> bootstrapRecord{};
        std::uint64_t generation = 0;
        bool busy = false;
        bool cancelled = false;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> calibrateChroma;
    ComPtr<ID3D11ComputeShader> calibrateLevels;
    ComPtr<ID3D11ComputeShader> demodShapeChroma;
    ComPtr<ID3D11ComputeShader> demodDesktopLevels;
    std::array<Slot, maximumSlots> slots;
    std::uint32_t slotCount = 0;
    DWORD ownerThread = 0;
    std::optional<ScreenCaptureDomain> activeDomain;
    std::array<float, maximumMetricCount> cpuMetrics{};
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> hard{};
    pbdesktoplevels::ReferenceChannel evaluator;
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

DemodStatus Demodulator::Create(ID3D11Device* device, const DemodConfig& config, std::unique_ptr<Demodulator>& output) noexcept
{
    if (device == nullptr || config.readbackSlotCount < minimumSlots || config.readbackSlotCount > maximumSlots)
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
    const auto residentSecond = residentFirst ?
        pbprotocol::CheckedAddUint64(residentFirst.Value(), pbmodulation::kDesktopLevelsMaximumDataBytes) : residentFirst;
    const auto residentThird = residentSecond ?
        pbprotocol::CheckedAddUint64(residentSecond.Value(), pbdesktoplevels::kProcessingReservationBytes) : residentSecond;
    const auto residentBytesResult = residentThird ? pbprotocol::CheckedAddUint64(residentThird.Value(), 1024ULL * 1024) : residentThird;
    if (!residentBytesResult || residentBytesResult.Value() > config.maximumResidentBytes)
    {
        return DemodStatus::Failure(DemodError::ResourceLimit, DemodStage::Configuration);
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
        DemodStatus status = GetAdapterLuid(device, state->snapshot.adapterLuid);
        if (!status)
        {
            return status;
        }
        const std::array<std::pair<const char*, ComPtr<ID3D11ComputeShader>*>, 4> shaders{{
            {"CalibrateChromaCS", std::addressof(state->calibrateChroma)},
            {"CalibrateLevelsCS", std::addressof(state->calibrateLevels)},
            {"DemodShapeChromaCS", std::addressof(state->demodShapeChroma)},
            {"DemodDesktopLevelsCS", std::addressof(state->demodDesktopLevels)}}};
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
            if (FAILED(native))
            {
                return DemodStatus::Failure(DemodError::NativeFailure, DemodStage::Resource, native);
            }
        }
        state->snapshot.residentBytes = residentBytesResult.Value();
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
        binding.rowTiles, binding.interleavePhase, binding.metricCount, 0, 0};
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
    context->CSSetShader(binding.mode == ProfileMode::ShapeChroma ? state.demodShapeChroma.Get() : state.demodDesktopLevels.Get(), nullptr, 0);
    context->Dispatch((binding.tileCount + 63) / 64, 1, 1);
    context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
    ID3D11ShaderResourceView* noInputs[]{nullptr, nullptr};
    context->CSSetShaderResources(0, 2, noInputs);
    ID3D11Buffer* noConstants[]{nullptr};
    context->CSSetConstantBuffers(0, 1, noConstants);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(slot.calibrationStaging.Get(), slot.calibration.Get());
    context->CopyResource(slot.metricsStaging.Get(), slot.metrics.Get());
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

DemodPollResult Demodulator::Poll(ID3D11DeviceContext* context, const DemodSubmission& submission, DemodFrameResult& output) noexcept
{
    if (!implementation_)
    {
        return {DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion), false};
    }
    auto& state = *implementation_;
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
        slot.frame.metadata.captureObservation != submission.captureObservation)
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
    const auto metrics = std::span(state.cpuMetrics).first(slot.binding.metricCount);
    if (!std::ranges::all_of(metrics, [](const float value) { return std::isfinite(value); }))
    {
        RetireSlot(state, slot, true);
        return {DemodStatus::Failure(DemodError::NonFiniteMetric, DemodStage::Readback), true};
    }
    std::fill_n(state.hard.begin(), slot.binding.dataBytes, std::byte{0});
    for (std::size_t bit = 0; bit < metrics.size(); bit++)
    {
        if (metrics[bit] < 0)
        {
            state.hard[bit / 8] |= static_cast<std::byte>(1u << (bit % 8));
        }
    }
    const auto metadata = slot.frame.metadata;
    const auto binding = slot.binding;
    const auto bootstrapRecord = slot.bootstrapRecord;
    RetireSlot(state, slot, false);

    DemodFrameResult result;
    result.metadata = metadata;
    result.visualProfileId = binding.profileId;
    result.metricReadbackBytes = metricBytes + calibrationBytes;
    result.evaluation = state.evaluator.EvaluateCodewords(bootstrapRecord, std::span(state.hard).first(binding.dataBytes), metrics);
    const auto accepted = state.evaluator.GetAcceptedTransportBlocks();
    result.acceptedTransportBlockCount = static_cast<std::uint32_t>(accepted.size());
    std::copy(accepted.begin(), accepted.end(), result.acceptedTransportBlocks.begin());
    {
        const std::lock_guard lock(state.snapshotMutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
        state.snapshot.metricReadbackBytes = pbprotocol::SaturatingAddUnsigned(state.snapshot.metricReadbackBytes, result.metricReadbackBytes);
    }
    output = result;
    return {{}, true};
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
