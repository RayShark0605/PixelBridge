#include "optional_diagnostic_fanout.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{

using pbcapturenormalize::CaptureConsumerCompletion;
using pbcapturenormalize::CaptureError;
using pbcapturenormalize::CaptureStage;
using pbcapturenormalize::CaptureStatus;

class ScriptedConsumer final : public pbcapturenormalize::ScreenCaptureConsumer
{
public:
    std::uint64_t ReservedBytes() const noexcept override
    {
        return reservedBytes;
    }

    CaptureStatus ValidateConfiguration(const pbcapturenormalize::CaptureConfig&) const noexcept override
    {
        validationCalls++;
        return validationStatus;
    }

    CaptureStatus DomainStarted(const pbcapturenormalize::ScreenCaptureDomain& domain,
        const pbcapturenormalize::CaptureEnvironment&, ID3D11Device*) override
    {
        domainStarts++;
        lastDomain = domain;
        if (throwOnDomainStart)
        {
            throw std::runtime_error("injected domain start failure");
        }
        return domainStatus;
    }

    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept override
    {
        invalidations++;
        invalidatedDomain = domain;
    }

    CaptureStatus Submit(const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext*) override
    {
        submits++;
        lastSubmitted = frame.metadata;
        if (throwOnSubmit)
        {
            throw std::runtime_error("injected submit failure");
        }
        return submitStatus;
    }

    CaptureConsumerCompletion CompleteStage(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        ID3D11Texture2D* const texture, ID3D11DeviceContext* const context, const bool cancelled) override
    {
        stagedCompletions++;
        lastCompleted = metadata;
        if (cancelled)
        {
            cancellations++;
            cancelledWithGpuObjects = cancelledWithGpuObjects || texture != nullptr || context != nullptr;
        }
        else
        {
            nonCancelledWithMissingGpuObjects = nonCancelledWithMissingGpuObjects || texture == nullptr || context == nullptr;
        }
        if (stageThrowsRemaining != 0)
        {
            stageThrowsRemaining--;
            throw std::runtime_error("injected staged completion failure");
        }
        if (nextCompletion < completions.size())
        {
            return completions[nextCompletion++];
        }
        return {};
    }

    CaptureStatus Completed(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        ID3D11DeviceContext*, const bool cancelled) override
    {
        legacyCompletions++;
        lastCompleted = metadata;
        legacyCancellation = legacyCancellation || cancelled;
        if (throwOnLegacyCompletion)
        {
            throw std::runtime_error("injected legacy completion failure");
        }
        return legacyStatus;
    }

    void Erased(const pbcapturenormalize::CaptureErasure&) noexcept override
    {
        erasures++;
    }

    std::uint64_t reservedBytes = 0;
    CaptureStatus validationStatus;
    CaptureStatus domainStatus;
    CaptureStatus submitStatus;
    CaptureStatus legacyStatus;
    std::vector<CaptureConsumerCompletion> completions;
    bool throwOnDomainStart = false;
    bool throwOnSubmit = false;
    bool throwOnLegacyCompletion = false;
    std::uint32_t stageThrowsRemaining = 0;
    mutable std::uint32_t validationCalls = 0;
    std::uint32_t domainStarts = 0;
    std::uint32_t invalidations = 0;
    std::uint32_t submits = 0;
    std::uint32_t stagedCompletions = 0;
    std::uint32_t legacyCompletions = 0;
    std::uint32_t cancellations = 0;
    std::uint32_t erasures = 0;
    std::size_t nextCompletion = 0;
    bool cancelledWithGpuObjects = false;
    bool nonCancelledWithMissingGpuObjects = false;
    bool legacyCancellation = false;
    pbcapturenormalize::ScreenCaptureDomain lastDomain;
    pbcapturenormalize::ScreenCaptureDomain invalidatedDomain;
    pbcapturenormalize::ScreenCaptureFrameMetadata lastSubmitted;
    pbcapturenormalize::ScreenCaptureFrameMetadata lastCompleted;
};

pbcapturenormalize::ScreenCaptureFrameMetadata MakeMetadata(const std::uint64_t observation = 1,
    const std::uint64_t slotGeneration = 1)
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    metadata.domain.sourceId[0] = std::byte{0x41};
    metadata.domain.captureEpoch = 7;
    metadata.captureObservation = observation;
    metadata.sourceGeneration = 3;
    metadata.slotGeneration = slotGeneration;
    metadata.slotIndex = 0;
    return metadata;
}

pbcapturenormalize::ScreenCaptureFrame MakeFrame(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata)
{
    pbcapturenormalize::ScreenCaptureFrame frame;
    frame.metadata = metadata;
    frame.texture = reinterpret_cast<ID3D11Texture2D*>(std::uintptr_t{0x1000});
    return frame;
}

ID3D11DeviceContext* DummyContext()
{
    return reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{0x2000});
}

} // namespace

TEST_CASE("Optional diagnostic fanout retains only branches that requested the bounded continuation",
    "[application][capture][fanout][staged-completion]")
{
    const auto primary = std::make_shared<ScriptedConsumer>();
    const auto diagnostic = std::make_shared<ScriptedConsumer>();
    primary->reservedBytes = 17;
    diagnostic->reservedBytes = 29;
    primary->completions = {{{}, true}, {{}, false}};
    diagnostic->completions = {{{}, false}};
    pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
    REQUIRE(fanout.ReservedBytes() == 46);
    pbcapturenormalize::CaptureConfig config;
    REQUIRE(fanout.ValidateConfiguration(config));
    REQUIRE(primary->validationCalls == 1);
    REQUIRE(diagnostic->validationCalls == 1);

    const auto metadata = MakeMetadata();
    pbcapturenormalize::CaptureEnvironment environment;
    REQUIRE(fanout.DomainStarted(metadata.domain, environment,
        reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
    const auto frame = MakeFrame(metadata);
    REQUIRE(fanout.Submit(frame, DummyContext()));
    const auto first = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
    REQUIRE(first.status);
    REQUIRE(first.gpuWorkSubmitted);
    REQUIRE(primary->stagedCompletions == 1);
    REQUIRE(diagnostic->stagedCompletions == 1);
    const auto second = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
    REQUIRE(second.status);
    REQUIRE_FALSE(second.gpuWorkSubmitted);
    REQUIRE(primary->stagedCompletions == 2);
    REQUIRE(diagnostic->stagedCompletions == 1);
    REQUIRE_FALSE(primary->nonCancelledWithMissingGpuObjects);
    REQUIRE_FALSE(diagnostic->nonCancelledWithMissingGpuObjects);
    REQUIRE(fanout.GetDiagnosticStatus());

    const auto duplicate = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
    REQUIRE(duplicate.status.code == CaptureError::InvalidFrame);
    REQUIRE(primary->stagedCompletions == 2);
    REQUIRE(diagnostic->stagedCompletions == 1);

    const auto replacementMetadata = MakeMetadata(2, 2);
    const auto replacementFrame = MakeFrame(replacementMetadata);
    REQUIRE(fanout.Submit(replacementFrame, DummyContext()));
    const auto cancelled = fanout.CompleteStage(replacementMetadata, nullptr, nullptr, true);
    REQUIRE(cancelled.status);
    REQUIRE_FALSE(cancelled.gpuWorkSubmitted);
    REQUIRE(primary->cancellations == 1);
    REQUIRE(diagnostic->cancellations == 1);
    REQUIRE_FALSE(primary->cancelledWithGpuObjects);
    REQUIRE_FALSE(diagnostic->cancelledWithGpuObjects);
    fanout.DomainInvalidated(metadata.domain);
    REQUIRE(primary->invalidations == 1);
    REQUIRE(diagnostic->invalidations == 1);
}

TEST_CASE("Optional diagnostic fanout records diagnostic failures without changing primary capture truth",
    "[application][capture][fanout][diagnostic][negative]")
{
    SECTION("diagnostic Submit failure removes that branch from completion")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        diagnostic->submitStatus = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer, 91);
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        const auto completion = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(completion.status);
        REQUIRE_FALSE(completion.gpuWorkSubmitted);
        REQUIRE(primary->stagedCompletions == 1);
        REQUIRE(diagnostic->stagedCompletions == 1);
        REQUIRE(diagnostic->cancellations == 1);
        REQUIRE_FALSE(diagnostic->cancelledWithGpuObjects);
        REQUIRE(fanout.GetDiagnosticStatus() == diagnostic->submitStatus);
    }

    SECTION("diagnostic exceptions remain observational and receive bounded cancellation cleanup")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        diagnostic->throwOnSubmit = true;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        const auto completion = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(completion.status);
        REQUIRE_FALSE(completion.gpuWorkSubmitted);
        REQUIRE(primary->stagedCompletions == 1);
        REQUIRE(diagnostic->stagedCompletions == 1);
        REQUIRE(diagnostic->cancellations == 1);
        REQUIRE_FALSE(diagnostic->cancelledWithGpuObjects);
        REQUIRE(fanout.GetDiagnosticStatus().code == CaptureError::ConsumerFailure);
    }

    SECTION("diagnostic DomainStarted exceptions disable only the optional branch")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        diagnostic->throwOnDomainStart = true;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        REQUIRE(fanout.CompleteStage(metadata, frame.texture, DummyContext(), false).status);
        REQUIRE(primary->stagedCompletions == 1);
        REQUIRE(diagnostic->submits == 0);
        REQUIRE(diagnostic->stagedCompletions == 0);
        REQUIRE(fanout.GetDiagnosticStatus().code == CaptureError::ConsumerFailure);
    }

    SECTION("diagnostic-only GPU work extends the lease but its status stays observational")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        const auto diagnosticFailure = CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Completion, 73);
        primary->completions = {{{}, false}};
        diagnostic->completions = {{diagnosticFailure, true}, {{}, false}};
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        const auto first = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(first.status);
        REQUIRE(first.gpuWorkSubmitted);
        REQUIRE(fanout.GetDiagnosticStatus() == diagnosticFailure);
        const auto second = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(second.status);
        REQUIRE_FALSE(second.gpuWorkSubmitted);
        REQUIRE(primary->stagedCompletions == 1);
        REQUIRE(diagnostic->stagedCompletions == 2);
    }

    SECTION("primary failure prevents diagnostic admission")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        primary->submitStatus = CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        REQUIRE_FALSE(fanout.Submit(MakeFrame(metadata), DummyContext()));
        REQUIRE(primary->submits == 1);
        REQUIRE(diagnostic->submits == 0);
        const auto completion = fanout.CompleteStage(metadata, nullptr, nullptr, true);
        REQUIRE(completion.status);
        REQUIRE_FALSE(completion.gpuWorkSubmitted);
        REQUIRE(primary->cancellations == 1);
    }

    SECTION("primary Submit exception stays authoritative and retains cancellation routing")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        primary->throwOnSubmit = true;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        REQUIRE(fanout.Submit(MakeFrame(metadata), DummyContext()).code == CaptureError::ConsumerFailure);
        const auto cleanup = fanout.CompleteStage(metadata, nullptr, nullptr, true);
        REQUIRE(cleanup.status);
        REQUIRE_FALSE(cleanup.gpuWorkSubmitted);
        REQUIRE(primary->cancellations == 1);
        REQUIRE(diagnostic->submits == 0);
        REQUIRE(diagnostic->stagedCompletions == 0);
    }
}

TEST_CASE("Optional diagnostic fanout preserves second-continuation cleanup and legacy no-GPU completion",
    "[application][capture][fanout][staged-completion][negative]")
{
    SECTION("a forbidden second continuation is retained only for terminal cancellation")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        primary->completions = {{{}, true}, {{}, true}, {{}, false}};
        diagnostic->completions = {{{}, false}};
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        REQUIRE(fanout.CompleteStage(metadata, frame.texture, DummyContext(), false).gpuWorkSubmitted);
        REQUIRE(fanout.CompleteStage(metadata, frame.texture, DummyContext(), false).gpuWorkSubmitted);
        const auto terminal = fanout.CompleteStage(metadata, nullptr, nullptr, true);
        REQUIRE(terminal.status);
        REQUIRE_FALSE(terminal.gpuWorkSubmitted);
        REQUIRE(primary->stagedCompletions == 3);
        REQUIRE(primary->cancellations == 1);
        REQUIRE(diagnostic->stagedCompletions == 1);
        REQUIRE_FALSE(primary->cancelledWithGpuObjects);
        REQUIRE(fanout.CompleteStage(metadata, nullptr, nullptr, true).status.code == CaptureError::InvalidFrame);
    }

    SECTION("wrong completion identity cannot consume the pending branch")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        auto wrong = metadata;
        wrong.captureObservation++;
        REQUIRE(fanout.CompleteStage(wrong, frame.texture, DummyContext(), false).status.code == CaptureError::InvalidFrame);
        REQUIRE(primary->stagedCompletions == 0);
        REQUIRE(diagnostic->stagedCompletions == 0);
        REQUIRE(fanout.CompleteStage(metadata, nullptr, nullptr, true).status);
        REQUIRE(primary->cancellations == 1);
        REQUIRE(diagnostic->cancellations == 1);
    }

    SECTION("primary completion exception conservatively retains one terminal cleanup")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        primary->stageThrowsRemaining = 1;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        const auto failed = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(failed.status.code == CaptureError::ConsumerFailure);
        REQUIRE(failed.gpuWorkSubmitted);
        REQUIRE(diagnostic->stagedCompletions == 1);
        const auto cleanup = fanout.CompleteStage(metadata, nullptr, nullptr, true);
        REQUIRE(cleanup.status);
        REQUIRE_FALSE(cleanup.gpuWorkSubmitted);
        REQUIRE(primary->cancellations == 1);
        REQUIRE(primary->stagedCompletions == 2);
        REQUIRE(diagnostic->stagedCompletions == 1);
    }

    SECTION("diagnostic completion exception cannot replace primary truth but retains its lease")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        diagnostic->stageThrowsRemaining = 1;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        const auto frame = MakeFrame(metadata);
        REQUIRE(fanout.Submit(frame, DummyContext()));
        const auto failed = fanout.CompleteStage(metadata, frame.texture, DummyContext(), false);
        REQUIRE(failed.status);
        REQUIRE(failed.gpuWorkSubmitted);
        REQUIRE(fanout.GetDiagnosticStatus().code == CaptureError::ConsumerFailure);
        const auto cleanup = fanout.CompleteStage(metadata, nullptr, nullptr, true);
        REQUIRE(cleanup.status);
        REQUIRE_FALSE(cleanup.gpuWorkSubmitted);
        REQUIRE(primary->stagedCompletions == 1);
        REQUIRE(diagnostic->stagedCompletions == 2);
        REQUIRE(diagnostic->cancellations == 1);
        REQUIRE_FALSE(diagnostic->cancelledWithGpuObjects);
    }

    SECTION("legacy completion never upgrades either branch to staged GPU work")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        diagnostic->legacyStatus = CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Completion, 44);
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        const auto metadata = MakeMetadata();
        REQUIRE(fanout.DomainStarted(metadata.domain, {},
            reinterpret_cast<ID3D11Device*>(std::uintptr_t{0x3000})));
        REQUIRE(fanout.Submit(MakeFrame(metadata), DummyContext()));
        REQUIRE(fanout.Completed(metadata, DummyContext(), false));
        REQUIRE(primary->legacyCompletions == 1);
        REQUIRE(diagnostic->legacyCompletions == 1);
        REQUIRE(primary->stagedCompletions == 0);
        REQUIRE(diagnostic->stagedCompletions == 0);
        REQUIRE(fanout.GetDiagnosticStatus() == diagnostic->legacyStatus);
    }

    SECTION("reservation overflow remains fail-closed")
    {
        const auto primary = std::make_shared<ScriptedConsumer>();
        const auto diagnostic = std::make_shared<ScriptedConsumer>();
        primary->reservedBytes = (std::numeric_limits<std::uint64_t>::max)();
        diagnostic->reservedBytes = 1;
        pbapp::detail::OptionalDiagnosticFanout fanout(primary, diagnostic);
        REQUIRE(fanout.ReservedBytes() == (std::numeric_limits<std::uint64_t>::max)());
    }
}
