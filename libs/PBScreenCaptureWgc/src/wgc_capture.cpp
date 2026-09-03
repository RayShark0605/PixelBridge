#include "capture_internal.h"
#include "../../PBCaptureNormalize/src/normalize_consumer.h"

namespace pbscreencapturewgc
{

struct WgcCapture::Implementation
{
    std::unique_ptr<detail::CaptureRuntime> runtime;
    std::shared_ptr<pbcapturenormalize::detail::NormalizeConsumer> normalizer;
};

CaptureStatus ValidateWgcCaptureConfig(const WgcCaptureConfig& config) noexcept
{
    if (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && config.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    return pbcapturenormalize::ValidateCaptureConfig(config);
}

WgcCapture::WgcCapture(std::shared_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

WgcCapture::~WgcCapture() = default;

CaptureStatus WgcCapture::Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer, std::unique_ptr<WgcCapture>& output) noexcept
{
    try
    {
        return WgcCaptureTestAccess::Create(config, std::move(consumer), detail::MakeNativeCaptureBackend(), output);
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

WgcCaptureSnapshot WgcCapture::GetSnapshot() const noexcept
{
    return implementation_->runtime->GetSnapshot();
}

pbcapturenormalize::CaptureNormalizeSnapshot WgcCapture::GetNormalizationSnapshot() const noexcept
{
    return implementation_->normalizer ? implementation_->normalizer->GetSnapshot() : pbcapturenormalize::CaptureNormalizeSnapshot{};
}

CaptureStatus WgcCapture::CreateNormalized(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                          std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer, std::unique_ptr<WgcCapture>& output,
                                          pbcapturenormalize::CaptureSnapshot* const failedStartSnapshot) noexcept
{
    if (failedStartSnapshot)
    {
        *failedStartSnapshot = {};
        failedStartSnapshot->shutdownComplete = true;
    }
    try
    {
        return WgcCaptureTestAccess::CreateNormalized(config, std::move(consumer), detail::MakeNativeCaptureBackend(), output, failedStartSnapshot);
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

CaptureStatus WgcCaptureTestAccess::CreateNormalized(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                                    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer,
                                                    std::unique_ptr<detail::CaptureBackend> backend, std::unique_ptr<WgcCapture>& output,
                                                    pbcapturenormalize::CaptureSnapshot* const failedStartSnapshot) noexcept
{
    if (failedStartSnapshot)
    {
        *failedStartSnapshot = {};
        failedStartSnapshot->shutdownComplete = true;
    }
    try
    {
        const auto implementation = std::make_shared<WgcCapture::Implementation>();
        const auto normalized = pbcapturenormalize::detail::NormalizeConsumer::Create(config, pbcapturenormalize::CaptureBackendKind::Wgc,
                                                                                    std::move(consumer), implementation->normalizer);
        if (!normalized)
        {
            return normalized;
        }
        auto created = std::unique_ptr<WgcCapture>(new WgcCapture(implementation));
        const auto status = detail::CaptureRuntime::Create(implementation->normalizer->GetRuntimeConfig(), implementation->normalizer,
                                                           std::move(backend), implementation->runtime, failedStartSnapshot);
        if (!status)
        {
            return status;
        }
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

void WgcCapture::RequestStop() noexcept
{
    implementation_->runtime->RequestStop();
}

CaptureStatus WgcCapture::Stop() noexcept
{
    return implementation_->runtime->Stop();
}

CaptureStatus WgcCaptureTestAccess::Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer,
                                         std::unique_ptr<detail::CaptureBackend> backend, std::unique_ptr<WgcCapture>& output) noexcept
{
    const auto validation = ValidateWgcCaptureConfig(config);
    if (!validation)
    {
        return validation;
    }
    try
    {
        const auto implementation = std::make_shared<WgcCapture::Implementation>();
        const auto status = detail::CaptureRuntime::Create(config, std::move(consumer), std::move(backend), implementation->runtime);
        if (!status)
        {
            return status;
        }
        auto capture = std::unique_ptr<WgcCapture>(new WgcCapture(implementation));
        output = std::move(capture);
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

void WgcCaptureTestAccess::RequestRecreate(WgcCapture& capture) noexcept
{
    capture.implementation_->runtime->RequestRecreate();
}

} // namespace pbscreencapturewgc
