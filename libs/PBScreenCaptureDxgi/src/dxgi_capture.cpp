#include "capture_internal.h"
#include "duplication_source.h"
#include "normalize_consumer.h"

#include <new>

namespace pbscreencapturedxgi
{
using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;

CaptureStatus ValidateDxgiCaptureConfig(const DxgiCaptureConfig& config) noexcept
{
    return detail::ValidateDuplicationPreflight(config, config.region);
}

DxgiCapture::DxgiCapture(std::unique_ptr<CaptureRuntime> runtime, std::shared_ptr<NormalizeConsumer> normalizer) noexcept
    : runtime_(std::move(runtime)), normalizer_(std::move(normalizer))
{
}

DxgiCapture::~DxgiCapture() = default;

CaptureStatus DxgiCapture::Create(const CaptureNormalizeConfig& config, std::shared_ptr<ScreenCaptureConsumer> consumer,
                                 std::unique_ptr<DxgiCapture>& output) noexcept
{
    try
    {
        detail::NativeDxgiOptions options;
        options.normalizeConfiguredFormat = true;
        return DxgiCaptureTestAccess::CreateNormalized(config, std::move(consumer), detail::MakeNativeDxgiBackend(options), output);
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

CaptureStatus DxgiCaptureTestAccess::CreateNormalized(const CaptureNormalizeConfig& config, std::shared_ptr<ScreenCaptureConsumer> consumer,
                                                     std::unique_ptr<CaptureBackend> backend, std::unique_ptr<DxgiCapture>& output) noexcept
{
    try
    {
        std::shared_ptr<NormalizeConsumer> normalizer;
        const auto normalized = NormalizeConsumer::Create(config, CaptureBackendKind::Dxgi, std::move(consumer), normalizer);
        if (!normalized)
        {
            return normalized;
        }
        const auto runtimeConfig = normalizer->GetRuntimeConfig();
        const auto validation = ValidateDxgiCaptureConfig(runtimeConfig);
        if (!validation)
        {
            return validation;
        }
        std::unique_ptr<CaptureRuntime> runtime;
        const auto status = CaptureRuntime::Create(runtimeConfig, normalizer, std::move(backend), runtime);
        if (!status)
        {
            return status;
        }
        auto created = std::unique_ptr<DxgiCapture>(new DxgiCapture(std::move(runtime), std::move(normalizer)));
        output = std::move(created);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

DxgiCaptureSnapshot DxgiCapture::GetSnapshot() const noexcept
{
    return runtime_->GetSnapshot();
}

CaptureNormalizeSnapshot DxgiCapture::GetNormalizationSnapshot() const noexcept
{
    return normalizer_ ? normalizer_->GetSnapshot() : CaptureNormalizeSnapshot{};
}

void DxgiCapture::RequestStop() noexcept
{
    runtime_->RequestStop();
}

CaptureStatus DxgiCapture::Stop() noexcept
{
    return runtime_->Stop();
}

CaptureStatus DxgiCaptureTestAccess::CreateRaw(const DxgiCaptureConfig& config, std::shared_ptr<RawRoiConsumer> consumer,
                                              std::unique_ptr<DxgiCapture>& output, const detail::NativeDxgiOptions& options) noexcept
{
    try
    {
        return CreateWithBackend(config, std::move(consumer), detail::MakeNativeDxgiBackend(options), output);
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

CaptureStatus DxgiCaptureTestAccess::CreateWithBackend(const DxgiCaptureConfig& config, std::shared_ptr<RawRoiConsumer> consumer,
                                                      std::unique_ptr<CaptureBackend> backend, std::unique_ptr<DxgiCapture>& output) noexcept
{
    const auto configStatus = ValidateDxgiCaptureConfig(config);
    if (!configStatus)
    {
        return configStatus;
    }
    try
    {
        std::unique_ptr<CaptureRuntime> runtime;
        const auto status = CaptureRuntime::Create(config, std::move(consumer), std::move(backend), runtime);
        if (!status)
        {
            return status;
        }
        auto created = std::unique_ptr<DxgiCapture>(new DxgiCapture(std::move(runtime)));
        output = std::move(created);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

void DxgiCaptureTestAccess::RequestRecreate(DxgiCapture& capture) noexcept
{
    capture.runtime_->RequestRecreate();
}

} // namespace pbscreencapturedxgi
