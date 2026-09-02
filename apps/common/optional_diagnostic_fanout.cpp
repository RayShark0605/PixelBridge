#include "optional_diagnostic_fanout.h"

#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace pbapp::detail
{

using pbcapturenormalize::CaptureConsumerCompletion;
using pbcapturenormalize::CaptureError;
using pbcapturenormalize::CaptureStage;
using pbcapturenormalize::CaptureStatus;
using pbcapturenormalize::ScreenCaptureFrameMetadata;

OptionalDiagnosticFanout::OptionalDiagnosticFanout(
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> primary,
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> diagnostic)
    : primary_(std::move(primary)), diagnostic_(std::move(diagnostic))
{
}

std::uint64_t OptionalDiagnosticFanout::ReservedBytes() const noexcept
{
    const auto total = pbprotocol::CheckedAddUint64(primary_->ReservedBytes(), diagnostic_->ReservedBytes());
    return total ? total.Value() : (std::numeric_limits<std::uint64_t>::max)();
}

CaptureStatus OptionalDiagnosticFanout::ValidateConfiguration(
    const pbcapturenormalize::CaptureConfig& config) const noexcept
{
    const auto primaryStatus = primary_->ValidateConfiguration(config);
    return primaryStatus ? diagnostic_->ValidateConfiguration(config) : primaryStatus;
}

CaptureStatus OptionalDiagnosticFanout::DomainStarted(
    const pbcapturenormalize::ScreenCaptureDomain& domain,
    const pbcapturenormalize::CaptureEnvironment& environment, ID3D11Device* const device)
{
    {
        const std::scoped_lock lock(pendingMutex_);
        if (std::ranges::any_of(pending_, [](const Pending& pending) { return pending.active; }))
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Recreate);
        }
    }
    CaptureStatus primaryStatus;
    try
    {
        primaryStatus = primary_->DomainStarted(domain, environment, device);
    }
    catch (...)
    {
        primaryStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    if (!primaryStatus)
    {
        return primaryStatus;
    }
    CaptureStatus diagnosticStatus;
    try
    {
        diagnosticStatus = diagnostic_->DomainStarted(domain, environment, device);
    }
    catch (...)
    {
        diagnosticStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    RecordDiagnosticStatus(diagnosticStatus);
    {
        const std::scoped_lock lock(pendingMutex_);
        diagnosticDomainActive_ = static_cast<bool>(diagnosticStatus);
    }
    return {};
}

void OptionalDiagnosticFanout::DomainInvalidated(
    const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept
{
    primary_->DomainInvalidated(domain);
    diagnostic_->DomainInvalidated(domain);
    const std::scoped_lock lock(pendingMutex_);
    diagnosticDomainActive_ = false;
}

CaptureStatus OptionalDiagnosticFanout::Submit(
    const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* const context)
{
    if (frame.metadata.slotIndex >= pending_.size())
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    {
        const std::scoped_lock lock(pendingMutex_);
        if (pending_[frame.metadata.slotIndex].active)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
    }
    CaptureStatus primaryStatus;
    try
    {
        primaryStatus = primary_->Submit(frame, context);
    }
    catch (...)
    {
        primaryStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    if (!primaryStatus)
    {
        const std::scoped_lock lock(pendingMutex_);
        auto& pending = pending_[frame.metadata.slotIndex];
        pending.active = true;
        pending.primaryPending = true;
        pending.metadata = frame.metadata;
        return primaryStatus;
    }
    bool diagnosticPending = false;
    {
        const std::scoped_lock lock(pendingMutex_);
        diagnosticPending = diagnosticDomainActive_;
    }
    bool diagnosticCancelled = false;
    if (diagnosticPending)
    {
        CaptureStatus diagnosticStatus;
        try
        {
            diagnosticStatus = diagnostic_->Submit(frame, context);
        }
        catch (...)
        {
            diagnosticStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
        }
        RecordDiagnosticStatus(diagnosticStatus);
        // The outer capture marker retires any work issued before a failed
        // diagnostic Submit. Keep the branch only for a null-object cancellation
        // callback so it cannot publish or extend the lease.
        diagnosticCancelled = !diagnosticStatus;
    }
    {
        const std::scoped_lock lock(pendingMutex_);
        auto& pending = pending_[frame.metadata.slotIndex];
        pending.active = true;
        pending.primaryPending = true;
        pending.diagnosticPending = diagnosticPending;
        pending.diagnosticCancelled = diagnosticCancelled;
        pending.metadata = frame.metadata;
    }
    return {};
}

CaptureConsumerCompletion OptionalDiagnosticFanout::CompleteStage(
    const ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* const texture,
    ID3D11DeviceContext* const context, const bool cancelled)
{
    if (metadata.slotIndex >= pending_.size())
    {
        return {CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion), false};
    }
    Pending pending;
    {
        const std::scoped_lock lock(pendingMutex_);
        const auto& stored = pending_[metadata.slotIndex];
        if (!stored.active || !SameCompletion(stored.metadata, metadata))
        {
            return {CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion), false};
        }
        pending = stored;
        pending_[metadata.slotIndex] = {};
    }

    CaptureConsumerCompletion primaryCompletion;
    if (pending.primaryPending)
    {
        try
        {
            primaryCompletion = primary_->CompleteStage(metadata, texture, context, cancelled);
        }
        catch (...)
        {
            primaryCompletion.status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            primaryCompletion.gpuWorkSubmitted = !cancelled;
        }
        if (cancelled && primaryCompletion.gpuWorkSubmitted)
        {
            primaryCompletion.status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            primaryCompletion.gpuWorkSubmitted = false;
        }
    }
    CaptureConsumerCompletion diagnosticCompletion;
    if (pending.diagnosticPending)
    {
        const bool diagnosticCancelled = cancelled || pending.diagnosticCancelled;
        try
        {
            diagnosticCompletion = diagnostic_->CompleteStage(metadata, diagnosticCancelled ? nullptr : texture,
                diagnosticCancelled ? nullptr : context, diagnosticCancelled);
        }
        catch (...)
        {
            diagnosticCompletion.status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            diagnosticCompletion.gpuWorkSubmitted = !diagnosticCancelled;
        }
        if (diagnosticCancelled && diagnosticCompletion.gpuWorkSubmitted)
        {
            diagnosticCompletion.status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            diagnosticCompletion.gpuWorkSubmitted = false;
        }
        RecordDiagnosticStatus(diagnosticCompletion.status);
    }
    const bool gpuWorkSubmitted = primaryCompletion.gpuWorkSubmitted || diagnosticCompletion.gpuWorkSubmitted;
    if (gpuWorkSubmitted && !cancelled)
    {
        const std::scoped_lock lock(pendingMutex_);
        auto& continuation = pending_[metadata.slotIndex];
        continuation.active = true;
        continuation.primaryPending = primaryCompletion.gpuWorkSubmitted;
        continuation.diagnosticPending = diagnosticCompletion.gpuWorkSubmitted;
        continuation.diagnosticCancelled = false;
        continuation.metadata = metadata;
    }
    return {primaryCompletion.status, gpuWorkSubmitted};
}

CaptureStatus OptionalDiagnosticFanout::Completed(
    const ScreenCaptureFrameMetadata& metadata, ID3D11DeviceContext* const context, const bool cancelled)
{
    if (metadata.slotIndex >= pending_.size())
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
    }
    Pending pending;
    {
        const std::scoped_lock lock(pendingMutex_);
        const auto& stored = pending_[metadata.slotIndex];
        if (!stored.active || !SameCompletion(stored.metadata, metadata))
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
        }
        pending = stored;
        pending_[metadata.slotIndex] = {};
    }
    CaptureStatus primaryStatus;
    if (pending.primaryPending)
    {
        try
        {
            primaryStatus = primary_->Completed(metadata, context, cancelled);
        }
        catch (...)
        {
            primaryStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
        }
    }
    if (pending.diagnosticPending)
    {
        const bool diagnosticCancelled = cancelled || pending.diagnosticCancelled;
        CaptureStatus diagnosticStatus;
        try
        {
            diagnosticStatus = diagnostic_->Completed(metadata, diagnosticCancelled ? nullptr : context, diagnosticCancelled);
        }
        catch (...)
        {
            diagnosticStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
        }
        RecordDiagnosticStatus(diagnosticStatus);
    }
    return primaryStatus;
}

void OptionalDiagnosticFanout::Erased(const pbcapturenormalize::CaptureErasure& erasure) noexcept
{
    primary_->Erased(erasure);
    diagnostic_->Erased(erasure);
}

CaptureStatus OptionalDiagnosticFanout::GetDiagnosticStatus() const noexcept
{
    const std::scoped_lock lock(diagnosticStatusMutex_);
    return diagnosticStatus_;
}

void OptionalDiagnosticFanout::RecordDiagnosticStatus(const CaptureStatus status) noexcept
{
    const std::scoped_lock lock(diagnosticStatusMutex_);
    if (!status && diagnosticStatus_)
    {
        diagnosticStatus_ = status;
    }
}

bool OptionalDiagnosticFanout::SameCompletion(
    const ScreenCaptureFrameMetadata& left, const ScreenCaptureFrameMetadata& right) noexcept
{
    return left.domain == right.domain && left.captureObservation == right.captureObservation &&
        left.sourceGeneration == right.sourceGeneration && left.slotIndex == right.slotIndex &&
        left.slotGeneration == right.slotGeneration;
}

} // namespace pbapp::detail
