#include "bootstrap_diagnostic_test_support.h"
#include "../../apps/PixelBridgeDecoder/capture_bootstrap_telemetry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

using namespace bootstrapdiagnostictest;

TEST_CASE("Diagnostic success uses committed pixel recovery even when the accepted event is evicted or never drained", "[bootstrap-diagnostic][bootstrap-cli]")
{
    const auto record = CanonicalRecord(9);
    const auto raster = Render(record);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    CHECK(GetBootstrapDiagnosticSuccessExitCode(processor.GetSnapshot()) == 4);
    const auto firstMetadata = Metadata(raster.size);
    REQUIRE(processor.Analyze(firstMetadata, raster.pixels, raster.RowPitch()));
    // A decoded but uncommitted candidate is not successful pixel recovery.
    CHECK(GetBootstrapDiagnosticSuccessExitCode(processor.GetSnapshot()) == 4);
    processor.Commit(firstMetadata);
    REQUIRE(processor.GetSnapshot().accepted == 1);
    // Also covers a last Commit after the CLI's last event drain but before
    // shutdown: no event has been read, yet the authoritative result is ready.
    CHECK(GetBootstrapDiagnosticSuccessExitCode(processor.GetSnapshot()) == 0);
    for (std::uint64_t observation = 2; observation <= 17; observation++)
    {
        const auto metadata = Metadata(raster.size, observation);
        REQUIRE(processor.Analyze(metadata, raster.pixels, raster.RowPitch()));
        processor.Commit(metadata);
    }
    const auto snapshot = processor.GetSnapshot();
    REQUIRE(snapshot.observations == 17);
    REQUIRE(snapshot.accepted == 1);
    REQUIRE(snapshot.duplicates == 16);
    REQUIRE(snapshot.diagnosticQueueDrops == 1);
    REQUIRE(snapshot.queuedEvents == 16);
    CHECK(GetBootstrapDiagnosticSuccessExitCode(snapshot) == 0);
    BootstrapDiagnosticEvent event;
    for (std::size_t index = 0; index < BootstrapDiagnosticProcessor::eventCapacity; index++)
    {
        REQUIRE(processor.TakeEvent(event));
        CHECK(event.disposition == BootstrapDisposition::DuplicatePixels);
        CHECK(event.visual.IsAccepted());
        CHECK(event.visual.canonical44 == record);
    }
    REQUIRE_FALSE(processor.TakeEvent(event));
    CHECK(GetBootstrapDiagnosticSuccessExitCode(processor.GetSnapshot()) == 0);
    // Shutdown clears current-domain state, not valid invocation counters.
    processor.Reset(std::nullopt);
    CHECK_FALSE(processor.GetSnapshot().domain.has_value());
    CHECK(processor.GetSnapshot().queuedEvents == 0);
    CHECK(GetBootstrapDiagnosticSuccessExitCode(processor.GetSnapshot()) == 0);
}

namespace
{
void RequireNoTemporalState(const BootstrapDiagnosticSnapshot& snapshot)
{
    CHECK(snapshot.geometryGeneration == 0);
    CHECK(snapshot.calibrationGeneration == 0);
    CHECK(snapshot.trackedSessions == 0);
    CHECK(snapshot.retainedSequences == 0);
}

struct DelayedControl
{
    void Release() noexcept
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        wake.notify_all();
    }
    void RecordThread() noexcept
    {
        const auto current = GetCurrentThreadId();
        const auto previous = workerThread.exchange(current);
        wrongThread = wrongThread || (previous != 0 && previous != current) || current == ownerThread;
    }
    const DWORD ownerThread = GetCurrentThreadId();
    std::mutex mutex;
    std::condition_variable wake;
    bool released = false;
    std::atomic<bool> blocked{false};
    std::atomic<bool> timedOut{false};
    std::atomic<bool> wrongThread{false};
    std::atomic<DWORD> workerThread{0};
    std::atomic<std::uint32_t> analyzes{0};
    std::atomic<std::uint32_t> commits{0};
    std::atomic<std::uint32_t> discards{0};
    std::array<ScreenCaptureDomain, 3> committedDomains{};
    std::array<std::uint64_t, 3> committedObservations{};
};

class DelayedProcessor final : public CpuFrameProcessor
{
public:
    DelayedProcessor(std::shared_ptr<BootstrapDiagnosticProcessor> processor, std::shared_ptr<DelayedControl> control)
        : processor_(std::move(processor)), control_(std::move(control))
    {
    }
    std::uint64_t ProcessingReservedBytes() const noexcept override
    {
        return processor_->ProcessingReservedBytes();
    }
    void Reset(std::optional<ScreenCaptureDomain> domain) noexcept override
    {
        control_->RecordThread();
        processor_->Reset(std::move(domain));
    }
    CaptureStatus Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch) override
    {
        control_->RecordThread();
        // Build the real candidate from the mapped pixels BEFORE blocking. The
        // wrapper changes scheduling only, never decode/admission decisions.
        const auto status = processor_->Analyze(metadata, pixels, rowPitch);
        const auto ordinal = control_->analyzes.fetch_add(1) + 1;
        if (ordinal == 2)
        {
            std::unique_lock lock(control_->mutex);
            control_->blocked = true;
            if (!control_->wake.wait_for(lock, std::chrono::seconds(10), [&] { return control_->released; }))
            {
                control_->timedOut = true;
                return CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Consumer);
            }
        }
        return status;
    }
    void Commit(const ScreenCaptureFrameMetadata& metadata) noexcept override
    {
        control_->RecordThread();
        processor_->Commit(metadata);
        const auto ordinal = control_->commits.load();
        if (ordinal < control_->committedDomains.size())
        {
            control_->committedDomains[ordinal] = metadata.domain;
            control_->committedObservations[ordinal] = metadata.captureObservation;
        }
        control_->commits++;
    }
    void Discard() noexcept override
    {
        control_->RecordThread();
        processor_->Discard();
        control_->discards++;
    }
private:
    const std::shared_ptr<BootstrapDiagnosticProcessor> processor_;
    const std::shared_ptr<DelayedControl> control_;
};

struct StopReadbackOnExit
{
    std::shared_ptr<DelayedControl> control;
    std::shared_ptr<DiagnosticCpuReadback> readback;
    ~StopReadbackOnExit()
    {
        control->Release();
        readback->RequestStop();
        static_cast<void>(readback->Stop(3000));
    }
};
}

TEST_CASE("Bootstrap diagnostic fixtures pin independent canonical bytes and only Commit accepts decoded pixels", "[bootstrap-diagnostic]")
{
    const auto record = CanonicalRecord();
    REQUIRE(Hex(record) == "504252470101000231305342444c425088776655443322110000000000000000000000000000000052044902");
    const auto raster = Render(record, false);
    const auto metadata = Metadata(raster.size);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(metadata.domain);
    REQUIRE(processor.Analyze(metadata, raster.pixels, raster.RowPitch()));
    RequireNoTemporalState(processor.GetSnapshot());
    CHECK(processor.GetSnapshot().observations == 0);
    CHECK(processor.GetSnapshot().queuedEvents == 0);
    const auto secondAnalyze = processor.Analyze(metadata, raster.pixels, raster.RowPitch());
    REQUIRE(secondAnalyze == CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer));
    processor.Commit(metadata);
    BootstrapDiagnosticEvent event;
    REQUIRE(processor.TakeEvent(event));
    RequireAccepted(event, record);
    CHECK(event.bootstrap.sessionTag.value == sessionTag);
    CHECK(event.bootstrap.frameSequence == 0);
    CHECK(event.bootstrap.visualProfileId == 0x50424C4442533031ULL);
    CHECK(event.bootstrap.visualLayoutVersion == 2);
    CHECK(event.capture.domain == metadata.domain);
    CHECK(event.capture.sourceGeneration == metadata.sourceGeneration);
    CHECK(event.geometryGeneration == 1);
    CHECK(event.calibrationGeneration == 1);
    CHECK(event.visual.geometry.originX == Catch::Approx(0).margin(1e-8));
    CHECK(event.visual.geometry.originY == Catch::Approx(0).margin(1e-8));
    CHECK(event.visual.geometry.scaleX == Catch::Approx(1).margin(1e-10));
    CHECK(event.visual.geometry.scaleY == Catch::Approx(1).margin(1e-10));
    const auto snapshot = processor.GetSnapshot();
    CHECK(snapshot.observations == 1);
    CHECK(snapshot.accepted == 1);
    CHECK(snapshot.duplicates == 0);
    CHECK(snapshot.erasures == 0);
    CHECK(snapshot.trackedSessions == 1);
    CHECK(snapshot.retainedSequences == 1);
    CHECK(snapshot.queuedEvents == 0);
    const auto lastRecord = event.visual.canonical44;
    REQUIRE_FALSE(processor.TakeEvent(event));
    CHECK(event.visual.canonical44 == lastRecord);
    CHECK(event.disposition == BootstrapDisposition::Accepted);
}

TEST_CASE("Bootstrap diagnostic discard and full original observation key forbid relabeling pending results", "[bootstrap-diagnostic]")
{
    const auto record = CanonicalRecord(7);
    const auto raster = Render(record);
    const auto original = Metadata(raster.size);
    for (std::uint32_t mutation = 0; mutation < 8; mutation++)
    {
        CAPTURE(mutation);
        BootstrapDiagnosticProcessor processor;
        processor.Reset(original.domain);
        REQUIRE(processor.Analyze(original, raster.pixels, raster.RowPitch()));
        auto changed = original;
        switch (mutation)
        {
        case 0: changed.domain.sourceId[0] ^= std::byte{1}; break;
        case 1: changed.domain.sourceId[7] ^= std::byte{1}; break;
        case 2: changed.domain.sourceId[15] ^= std::byte{1}; break;
        case 3: changed.domain.captureEpoch++; break;
        case 4: changed.sourceGeneration++; break;
        case 5: changed.captureObservation++; break;
        case 6: changed.slotIndex++; break;
        case 7: changed.slotGeneration++; break;
        }
        processor.Commit(changed);
        auto snapshot = processor.GetSnapshot();
        CHECK(snapshot.staleCommits == 1);
        CHECK(snapshot.discardedCandidates == 1);
        CHECK(snapshot.observations == 0);
        CHECK(snapshot.queuedEvents == 0);
        RequireNoTemporalState(snapshot);
        processor.Commit(original);
        CHECK(processor.GetSnapshot().staleCommits == 2);
        RequireAccepted(Observe(processor, raster, original), record);
        CHECK(processor.GetSnapshot().accepted == 1);
    }
    BootstrapDiagnosticProcessor processor;
    processor.Reset(original.domain);
    REQUIRE(processor.Analyze(original, raster.pixels, raster.RowPitch()));
    processor.Discard();
    processor.Discard();
    CHECK(processor.GetSnapshot().discardedCandidates == 1);
    processor.Commit(original);
    CHECK(processor.GetSnapshot().staleCommits == 1);
    RequireNoTemporalState(processor.GetSnapshot());
    RequireAccepted(Observe(processor, raster, original), record);
    REQUIRE(processor.Analyze(original, raster.pixels, raster.RowPitch()));
    processor.Commit(original);
    CHECK(processor.GetSnapshot().staleCommits == 2);
    CHECK(processor.GetSnapshot().accepted == 1);
    CHECK(processor.GetSnapshot().duplicates == 0);
}

TEST_CASE("Bootstrap diagnostic reset clears only visual-domain histories geometry calibration pending and queued output", "[bootstrap-diagnostic]")
{
    const auto record = CanonicalRecord();
    const auto raster = Render(record);
    const auto oldMetadata = Metadata(raster.size);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(oldMetadata.domain);
    REQUIRE(processor.Analyze(oldMetadata, raster.pixels, raster.RowPitch()));
    processor.Commit(oldMetadata);
    REQUIRE(processor.GetSnapshot().queuedEvents == 1);
    const auto pending = Metadata(raster.size, 2);
    REQUIRE(processor.Analyze(pending, raster.pixels, raster.RowPitch()));
    processor.Reset(std::nullopt);
    auto snapshot = processor.GetSnapshot();
    CHECK_FALSE(snapshot.domain);
    CHECK(snapshot.resets == 2);
    CHECK(snapshot.discardedCandidates == 1);
    CHECK(snapshot.diagnosticQueueDrops == 1);
    CHECK(snapshot.queuedEvents == 0);
    CHECK(snapshot.accepted == 1);
    RequireNoTemporalState(snapshot);
    processor.Commit(pending);
    CHECK(processor.GetSnapshot().staleCommits == 1);
    BootstrapDiagnosticEvent event;
    REQUIRE_FALSE(processor.TakeEvent(event));

    // Sender identity/sequence stay unchanged. Only local capture identity or
    // epoch changes; protocol/FEC objects are not arguments to this processor.
    for (const auto domain : {Domain(2), Domain(2, 31)})
    {
        processor.Reset(domain);
        RequireNoTemporalState(processor.GetSnapshot());
        const auto current = Metadata(raster.size, 1, domain);
        RequireAccepted(Observe(processor, raster, current), record);
        CHECK(processor.GetSnapshot().domain == domain);
        CHECK(processor.GetSnapshot().geometryGeneration == 1);
        CHECK(processor.GetSnapshot().calibrationGeneration == 1);
        CHECK(processor.GetSnapshot().retainedSequences == 1);
        CHECK(processor.GetSnapshot().trackedSessions == 1);
    }
    CHECK(processor.GetSnapshot().accepted == 3);
    CHECK(processor.GetSnapshot().duplicates == 0);
    CHECK(processor.GetSnapshot().identityConflicts == 0);
}

TEST_CASE("Bootstrap diagnostic duplicate pixels observations geometry and calibration remain separate generations", "[bootstrap-diagnostic]")
{
    const auto record = CanonicalRecord(3);
    const auto raster = Render(record);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    RequireAccepted(Observe(processor, raster, Metadata(raster.size, 1)), record);
    auto event = Observe(processor, raster, Metadata(raster.size, 2));
    CHECK(event.disposition == BootstrapDisposition::DuplicatePixels);
    CHECK(event.geometryGeneration == 1);
    CHECK(event.calibrationGeneration == 1);

    auto changedPixel = raster;
    const std::size_t offset = (200 * 960 + 200) * 4; // Logical (400,400) is reserved background, not a marker or timing patch.
    for (std::size_t channel = 0; channel < 3; channel++)
    {
        REQUIRE(changedPixel.pixels[offset + channel] == std::byte{128});
        changedPixel.pixels[offset + channel] = std::byte{129};
    }
    const auto originalDigest = event.pixelDigest;
    event = Observe(processor, changedPixel, Metadata(changedPixel.size, 3));
    REQUIRE(event.visual.IsAccepted());
    CHECK(event.disposition == BootstrapDisposition::DuplicateObservation);
    CHECK(event.pixelDigest != originalDigest);
    CHECK(event.geometryGeneration == 1);
    CHECK(event.calibrationGeneration == 1);
    const auto recalibrated = Recalibrate(raster);
    event = Observe(processor, recalibrated, Metadata(recalibrated.size, 4));
    REQUIRE(event.visual.IsAccepted());
    CHECK(event.disposition == BootstrapDisposition::DuplicateCalibrationChanged);
    CHECK(event.geometryGeneration == 1);
    CHECK(event.calibrationGeneration == 2);
    CHECK(event.visual.blackLevel == Catch::Approx(40).margin(1e-8));
    CHECK(event.visual.whiteLevel == Catch::Approx(216).margin(1e-8));
    const auto translated = Translate(recalibrated);
    event = Observe(processor, translated, Metadata(translated.size, 5));
    REQUIRE(event.visual.IsAccepted());
    CHECK(event.disposition == BootstrapDisposition::DuplicateGeometryChanged);
    CHECK(event.geometryGeneration == 2);
    CHECK(event.calibrationGeneration == 2);
    CHECK(event.visual.geometry.originX == Catch::Approx(13).margin(1e-8));
    CHECK(event.visual.geometry.originY == Catch::Approx(11).margin(1e-8));
    CHECK(event.visual.geometry.scaleX == Catch::Approx(0.5).margin(1e-10));
    CHECK(event.visual.geometry.scaleY == Catch::Approx(0.5).margin(1e-10));
    event = Observe(processor, translated, Metadata(translated.size, 6));
    CHECK(event.disposition == BootstrapDisposition::DuplicatePixels);
    CHECK(event.geometryGeneration == 2);
    CHECK(event.calibrationGeneration == 2);
    CHECK(processor.GetSnapshot().accepted == 1);
    CHECK(processor.GetSnapshot().duplicates == 5);
    CHECK(processor.GetSnapshot().retainedSequences == 1);
}

TEST_CASE("Bootstrap diagnostic identity conflict tombstones survive replays and bounded history eviction remains stale", "[bootstrap-diagnostic]")
{
    static_assert(BootstrapDiagnosticProcessor::historyCapacity == 64);
    static_assert(BootstrapDiagnosticProcessor::eventCapacity == 16);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    const auto initialRecord = CanonicalRecord();
    const auto conflictingRecord = CanonicalRecord(0, sessionTag, 1);
    REQUIRE(initialRecord != conflictingRecord);
    REQUIRE(pbprotocol::ParseBootstrapRecord(initialRecord));
    REQUIRE(pbprotocol::ParseBootstrapRecord(conflictingRecord));
    const auto original = Render(initialRecord);
    const auto conflicting = Render(conflictingRecord);
    std::uint64_t observation = 1;
    RequireAccepted(Observe(processor, original, Metadata(original.size, observation++)), initialRecord);
    for (const Raster* raster : {&conflicting, &original, &conflicting})
    {
        const auto event = Observe(processor, *raster, Metadata(raster->size, observation++));
        REQUIRE(event.visual.IsAccepted());
        CHECK(event.disposition == BootstrapDisposition::IdentityConflict);
        CHECK(event.geometryGeneration == 1);
        CHECK(event.calibrationGeneration == 1);
    }
    CHECK(processor.GetSnapshot().identityConflicts == 3);
    CHECK(processor.GetSnapshot().accepted == 1);
    CHECK(processor.GetSnapshot().retainedSequences == 1);

    for (std::uint64_t sequence = 1; sequence <= 64; sequence++)
    {
        CAPTURE(sequence);
        const auto raster = Render(CanonicalRecord(sequence));
        const auto metadata = Metadata(raster.size, observation++);
        REQUIRE(processor.Analyze(metadata, raster.pixels, raster.RowPitch()));
        processor.Commit(metadata);
        REQUIRE(processor.GetSnapshot().lastDisposition == BootstrapDisposition::Accepted);
    }
    auto snapshot = processor.GetSnapshot();
    CHECK(snapshot.accepted == 65);
    CHECK(snapshot.retainedSequences == 64);
    CHECK(snapshot.trackedSessions == 1);
    CHECK(snapshot.queuedEvents == 16);
    CHECK(snapshot.diagnosticQueueDrops == 48);
    for (std::uint64_t sequence = 49; sequence <= 64; sequence++)
    {
        BootstrapDiagnosticEvent event;
        REQUIRE(processor.TakeEvent(event));
        CHECK(event.disposition == BootstrapDisposition::Accepted);
        CHECK(event.bootstrap.frameSequence == sequence);
        CHECK(event.visual.canonical44 == CanonicalRecord(sequence));
    }
    CHECK(processor.GetSnapshot().queuedEvents == 0);
    const auto stale = Observe(processor, original, Metadata(original.size, observation++));
    CHECK(stale.disposition == BootstrapDisposition::StaleSequence);
    const auto latest = Render(CanonicalRecord(64));
    const auto repeated = Observe(processor, latest, Metadata(latest.size, observation++));
    CHECK(repeated.disposition == BootstrapDisposition::DuplicatePixels);
    snapshot = processor.GetSnapshot();
    CHECK(snapshot.accepted == 65);
    CHECK(snapshot.duplicates == 1);
    CHECK(snapshot.erasures == 4);
    CHECK(snapshot.identityConflicts == 3);
    CHECK(snapshot.retainedSequences == 64);
    CHECK(snapshot.geometryGeneration == 1);
    CHECK(snapshot.calibrationGeneration == 1);
}

TEST_CASE("Bootstrap diagnostic eight-session cap is fail closed without evicting an admitted sender", "[bootstrap-diagnostic]")
{
    static_assert(BootstrapDiagnosticProcessor::sessionCapacity == 8);
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    for (std::uint64_t index = 0; index < 9; index++)
    {
        CAPTURE(index);
        const auto record = CanonicalRecord(0, sessionTag + index);
        const auto raster = Render(record);
        const auto event = Observe(processor, raster, Metadata(raster.size, index + 1));
        REQUIRE(event.visual.IsAccepted());
        if (index < 8)
        {
            RequireAccepted(event, record);
        }
        else
        {
            CHECK(event.disposition == BootstrapDisposition::SessionLimit);
        }
    }
    auto snapshot = processor.GetSnapshot();
    CHECK(snapshot.trackedSessions == 8);
    CHECK(snapshot.retainedSequences == 8);
    CHECK(snapshot.accepted == 8);
    CHECK(snapshot.erasures == 1);
    CHECK(snapshot.geometryGeneration == 1);
    CHECK(snapshot.calibrationGeneration == 1);
    const auto existing = Render(CanonicalRecord(1));
    RequireAccepted(Observe(processor, existing, Metadata(existing.size, 10)), CanonicalRecord(1));
    CHECK(processor.GetSnapshot().trackedSessions == 8);
    processor.Reset(Domain(2));
    const auto formerlyLimited = Render(CanonicalRecord(0, sessionTag + 8));
    RequireAccepted(Observe(processor, formerlyLimited, Metadata(formerlyLimited.size, 1, Domain(2))), CanonicalRecord(0, sessionTag + 8));
    snapshot = processor.GetSnapshot();
    CHECK(snapshot.trackedSessions == 1);
    CHECK(snapshot.retainedSequences == 1);
    CHECK(snapshot.accepted == 10);
    CHECK(snapshot.erasures == 1);
}

TEST_CASE("Bootstrap diagnostic BGRA R10 and declared linear SDR FP16 all decode actual format-preserved pixels", "[bootstrap-diagnostic]")
{
    const auto record = CanonicalRecord(19);
    const auto raster = Render(record);
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        CAPTURE(format);
        const std::size_t pixelBytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        std::vector<std::byte> pixels(static_cast<std::size_t>(raster.size.width) * raster.size.height * pixelBytes);
        bool validPalette = true;
        for (std::size_t pixel = 0; pixel < raster.pixels.size() / 4; pixel++)
        {
            const auto level = std::to_integer<std::uint32_t>(raster.pixels[pixel * 4]);
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                std::memcpy(pixels.data() + pixel * 4, raster.pixels.data() + pixel * 4, 4);
            }
            else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                const std::uint32_t code = (level * 1023 + 127) / 255;
                const std::uint32_t packed = code | (code << 10) | (code << 20) | 0xc0000000u;
                for (std::size_t index = 0; index < 4; index++)
                {
                    pixels[pixel * 4 + index] = static_cast<std::byte>((packed >> (index * 8)) & 255u);
                }
            }
            else
            {
                validPalette = validPalette && (level == 32 || level == 128 || level == 224);
                // Exact half-float fixture values: 1/64, 7/32 and 3/4 are
                // finite linear SDR gray levels, NOT HDR or a decoder oracle.
                const std::uint16_t gray = level == 32 ? 0x2400u : level == 128 ? 0x3300u : 0x3a00u;
                for (std::size_t channel = 0; channel < 4; channel++)
                {
                    const std::uint16_t component = channel == 3 ? 0x3c00u : gray;
                    pixels[pixel * 8 + channel * 2] = static_cast<std::byte>(component & 255u);
                    pixels[pixel * 8 + channel * 2 + 1] = static_cast<std::byte>(component >> 8);
                }
            }
        }
        REQUIRE(validPalette);
        auto metadata = Metadata(raster.size);
        metadata.pixelFormat = format;
        metadata.sourcePixelFormat = format;
        metadata.bitsPerColor = format == DXGI_FORMAT_B8G8R8A8_UNORM ? 8 : format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10 : 16;
        metadata.signalEncoding = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? CaptureSignalEncoding::LinearScRgb : CaptureSignalEncoding::SdrRgb;
        BootstrapDiagnosticProcessor processor;
        processor.Reset(metadata.domain);
        REQUIRE(processor.Analyze(metadata, pixels, static_cast<std::size_t>(raster.size.width) * pixelBytes));
        processor.Commit(metadata);
        BootstrapDiagnosticEvent event;
        REQUIRE(processor.TakeEvent(event));
        RequireAccepted(event, record);
        CHECK(event.capture.pixelFormat == format);
        CHECK(event.capture.sourcePixelFormat == format);
        CHECK(processor.GetSnapshot().accepted == 1);
        CHECK(processor.GetSnapshot().erasures == 0);
    }
}

TEST_CASE("Bootstrap diagnostic unsupported color and HDR never reach visual admission", "[bootstrap-diagnostic][bootstrap-diagnostic-fast]")
{
    for (std::uint32_t mutation = 0; mutation < 9; mutation++)
    {
        CAPTURE(mutation);
        BootstrapDiagnosticProcessor processor;
        processor.Reset(Domain());
        auto metadata = Metadata({1, 1});
        switch (mutation)
        {
        case 0: metadata.hdr = true; break;
        case 1: metadata.signalEncoding = CaptureSignalEncoding::Unknown; break;
        case 2: metadata.pixelFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case 3: metadata.signalEncoding = CaptureSignalEncoding::LinearScRgb; break;
        case 4: metadata.pixelFormat = DXGI_FORMAT_R10G10B10A2_UNORM; metadata.signalEncoding = CaptureSignalEncoding::LinearScRgb; break;
        case 5: metadata.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case 6: metadata.pixelFormat = DXGI_FORMAT_UNKNOWN; break;
        case 7: metadata.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; metadata.signalEncoding = CaptureSignalEncoding::LinearScRgb; metadata.hdr = true; break;
        case 8: metadata.pixelFormat = DXGI_FORMAT_R10G10B10A2_UNORM; metadata.hdr = true; break;
        }
        REQUIRE(processor.Analyze(metadata, {}, 0));
        processor.Commit(metadata);
        BootstrapDiagnosticEvent event;
        REQUIRE(processor.TakeEvent(event));
        CHECK(event.disposition == BootstrapDisposition::UnsupportedSignal);
        CHECK_FALSE(event.visual.IsAccepted());
        CHECK(processor.GetSnapshot().accepted == 0);
        CHECK(processor.GetSnapshot().erasures == 1);
        RequireNoTemporalState(processor.GetSnapshot());
    }
}

TEST_CASE("Bootstrap diagnostic cursor physical dimensions packed pitch and spans are validated before pixel access", "[bootstrap-diagnostic][bootstrap-diagnostic-fast]")
{
    const std::array<std::byte, 8> pixels{};
    for (std::uint32_t mutation = 0; mutation < 15; mutation++)
    {
        CAPTURE(mutation);
        BootstrapDiagnosticProcessor processor;
        processor.Reset(Domain());
        auto metadata = Metadata({1, 1});
        std::size_t rowPitch = 4;
        auto view = std::span<const std::byte>(pixels).first(4);
        switch (mutation)
        {
        case 0: metadata.isCursorExcluded = false; break;
        case 1: metadata.roiSize.width = 0; break;
        case 2: metadata.roiSize.height = -1; break;
        case 3: metadata.roiSize.width = std::numeric_limits<std::int32_t>::max(); break;
        case 4: metadata.roiSize.height = 16385; break;
        case 5: metadata.physicalRoi.right++; break;
        case 6: metadata.physicalRoi.left = std::numeric_limits<LONG>::min(); metadata.physicalRoi.right = std::numeric_limits<LONG>::max(); break;
        case 7: metadata.sourceGeneration = 0; break;
        case 8: metadata.slotGeneration = 0; break;
        case 9: rowPitch = 3; break;
        case 10: rowPitch = 8; view = pixels; break;
        case 11: rowPitch = std::numeric_limits<std::size_t>::max(); break;
        case 12: view = view.first(3); break;
        case 13: view = pixels; break;
        case 14: metadata.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; metadata.signalEncoding = CaptureSignalEncoding::LinearScRgb; break;
        }
        REQUIRE(processor.Analyze(metadata, view, rowPitch));
        processor.Commit(metadata);
        BootstrapDiagnosticEvent event;
        REQUIRE(processor.TakeEvent(event));
        CHECK(event.disposition == BootstrapDisposition::InvalidMetadata);
        CHECK_FALSE(event.visual.IsAccepted());
        CHECK(processor.GetSnapshot().erasures == 1);
        RequireNoTemporalState(processor.GetSnapshot());
    }
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    auto metadata = Metadata({1, 1});
    metadata.captureObservation = 0;
    REQUIRE(processor.Analyze(metadata, pixels, 4));
    processor.Commit(metadata);
    CHECK(processor.GetSnapshot().staleCommits == 1);
    CHECK(processor.GetSnapshot().observations == 0);
    RequireNoTemporalState(processor.GetSnapshot());
}

TEST_CASE("Bootstrap diagnostic bad visual input is an erasure rather than an accepted empty record", "[bootstrap-diagnostic]")
{
    const auto raster = Render(CanonicalRecord());
    auto invalid = raster;
    std::fill(invalid.pixels.begin(), invalid.pixels.end(), std::byte{128});
    for (std::size_t index = 3; index < invalid.pixels.size(); index += 4)
    {
        invalid.pixels[index] = std::byte{255};
    }
    BootstrapDiagnosticProcessor processor;
    processor.Reset(Domain());
    const auto event = Observe(processor, invalid, Metadata(invalid.size));
    CHECK(event.disposition == BootstrapDisposition::VisualErasure);
    CHECK_FALSE(event.visual.IsAccepted());
    CHECK(processor.GetSnapshot().erasures == 1);
    RequireNoTemporalState(processor.GetSnapshot());
    RequireAccepted(Observe(processor, raster, Metadata(raster.size, 2)), CanonicalRecord());
}

TEST_CASE("Actual WARP readback and Bootstrap processor discard a decoded old-epoch candidate before committing new pixels", "[bootstrap-diagnostic][bootstrap-diagnostic-readback]")
{
    const bool desktopLevels = GENERATE(false, true);
    const auto MakeRecord = [desktopLevels](const std::uint64_t sequence)
    {
        if (!desktopLevels)
        {
            return CanonicalRecord(sequence);
        }
        pbprotocol::BootstrapRecord record;
        record.visualLayoutVersion = pbmodulation::kDesktopLevelsLayoutVersion;
        record.protocolVersion = pbprotocol::GetProtocolVersion();
        record.visualProfileId = pbmodulation::kDesktopLevels4ProfileId;
        record.sessionTag.value = sessionTag;
        record.frameSequence = sequence;
        std::array<std::byte, 44> bytes{};
        REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
        return bytes;
    };
    const auto MakeRaster = [desktopLevels](const std::array<std::byte, 44>& record)
    {
        if (!desktopLevels)
        {
            return Render(record);
        }
        Raster raster{{1920, 1080}, std::vector<std::byte>(1920 * 1080 * 4)};
        std::vector<std::byte> data(21672);
        REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, data, raster.pixels));
        return raster;
    };
    const auto firstRecord = MakeRecord(0);
    const auto oldPendingRecord = MakeRecord(1);
    const auto first = MakeRaster(firstRecord);
    const auto oldPending = MakeRaster(oldPendingRecord);
    const auto oldDomain = Domain();
    const auto newDomain = Domain(2);
    Graphics graphics;
    auto firstMetadata = Metadata(first.size, 1, oldDomain);
    firstMetadata.adapterLuid = graphics.adapterLuid;
    const auto environment = Environment(firstMetadata);
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    if (desktopLevels)
    {
        REQUIRE(BootstrapDiagnosticProcessor::CreateDesktopLevels(processor));
    }
    else
    {
        processor = std::make_shared<BootstrapDiagnosticProcessor>();
    }
    const auto control = std::make_shared<DelayedControl>();
    DiagnosticReadbackConfig config;
    config.maximumRoiSize = first.size;
    config.maximumFrameAgeMilliseconds = 60000;
    config.processingReservedBytes = processor->ProcessingReservedBytes();
    config.maximumReadbackBytes = static_cast<std::uint64_t>(first.size.width) * first.size.height * 8 * 6 + config.processingReservedBytes;
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(config, std::make_shared<DelayedProcessor>(processor, control), readback));
    const StopReadbackOnExit cleanup{control, readback};
    REQUIRE(readback->DomainStarted(oldDomain, environment, graphics.device.Get()));
    Deliver(graphics, *readback, first, firstMetadata);
    REQUIRE(Await([&] { return control->commits == 1; }));
    REQUIRE(processor->GetSnapshot().accepted == 1);
    REQUIRE(processor->GetSnapshot().geometryGeneration == 1);
    REQUIRE(processor->GetSnapshot().calibrationGeneration == 1);
    Deliver(graphics, *readback, oldPending, Metadata(oldPending.size, 2, oldDomain));
    REQUIRE(Await([&] { return control->blocked.load(); }));
    REQUIRE(control->analyzes == 2);
    CHECK(processor->GetSnapshot().accepted == 1);
    CHECK(processor->GetSnapshot().observations == 1);

    readback->DomainInvalidated(oldDomain);
    REQUIRE_FALSE(readback->GetSnapshot().active);
    REQUIRE(readback->DomainStarted(newDomain, environment, graphics.device.Get()));
    auto newMetadata = Metadata(first.size, 1, newDomain);
    newMetadata.sourceGeneration = 24;
    Deliver(graphics, *readback, first, newMetadata);
    REQUIRE(readback->GetSnapshot().mappedFrames == 3);
    REQUIRE(readback->GetSnapshot().queuedFrames == 1);
    CHECK(control->commits == 1);

    // The owner has changed domains, but the real CPU worker is still blocked
    // after analyzing old pixels. Report each stage's domain rather than attach
    // the old committed geometry/calibration to the replacement capture epoch.
    CaptureSnapshot captureSnapshot;
    captureSnapshot.state = CaptureState::Running;
    captureSnapshot.environment = environment;
    CaptureNormalizeSnapshot normalizedSnapshot;
    normalizedSnapshot.active = true;
    normalizedSnapshot.domain = newDomain;
    const auto waitingReadback = readback->GetSnapshot();
    const auto waitingVisual = processor->GetSnapshot();
    REQUIRE(waitingReadback.domain == newDomain);
    REQUIRE(waitingReadback.active);
    REQUIRE(waitingReadback.resetPending);
    REQUIRE(waitingVisual.domain == oldDomain);
    CHECK(waitingVisual.geometryGeneration == 1);
    CHECK(waitingVisual.calibrationGeneration == 1);
    const auto oldDomainJson = "{\"sourceId\":\"" + Hex(oldDomain.sourceId) + "\",\"captureEpoch\":\"1\"}";
    const auto newDomainJson = "{\"sourceId\":\"" + Hex(newDomain.sourceId) + "\",\"captureEpoch\":\"2\"}";
    const auto waitingJson = pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", captureSnapshot, normalizedSnapshot, waitingReadback, waitingVisual, 0, 1);
    CHECK(waitingJson.find("\"readback\":{\"domain\":" + newDomainJson + ",\"active\":true,\"resetPending\":true") != std::string::npos);
    CHECK(waitingJson.find("\"visual\":{\"domain\":" + oldDomainJson + ",\"temporalStateCurrent\":false") != std::string::npos);
    control->Release();
    REQUIRE(Await([&] { return control->commits == 2; }));
    auto snapshot = processor->GetSnapshot();
    CHECK(snapshot.domain == newDomain);
    CHECK(snapshot.observations == 2);
    CHECK(snapshot.accepted == 2);
    CHECK(snapshot.duplicates == 0);
    CHECK(snapshot.identityConflicts == 0);
    CHECK(snapshot.discardedCandidates == 1);
    CHECK(snapshot.staleCommits == 0);
    CHECK(snapshot.trackedSessions == 1);
    CHECK(snapshot.retainedSequences == 1);
    CHECK(snapshot.geometryGeneration == 1);
    CHECK(snapshot.calibrationGeneration == 1);
    CHECK(snapshot.queuedEvents == (desktopLevels ? 2u : 1u));
    CHECK(snapshot.diagnosticQueueDrops == (desktopLevels ? 0u : 1u));
    BootstrapDiagnosticEvent event;
    if (desktopLevels)
    {
        REQUIRE(processor->TakeEvent(event));
        RequireAccepted(event, firstRecord);
        REQUIRE(event.capture.domain == oldDomain);
        REQUIRE(event.levels.evaluation.IsVerified());
    }
    REQUIRE(processor->TakeEvent(event));
    RequireAccepted(event, firstRecord);
    CHECK(event.capture.domain == newDomain);
    CHECK(event.capture.captureObservation == 1);
    CHECK(event.bootstrap.frameSequence == 0);
    CHECK(control->committedDomains[0] == oldDomain);
    CHECK(control->committedObservations[0] == 1);
    CHECK(control->committedDomains[1] == newDomain);
    CHECK(control->committedObservations[1] == 1);
    CHECK(control->discards == 1);
    CHECK_FALSE(control->timedOut.load());
    CHECK_FALSE(control->wrongThread.load());
    CHECK(control->workerThread != 0);
    const auto readbackSnapshot = readback->GetSnapshot();
    CHECK(readbackSnapshot.error);
    CHECK(readbackSnapshot.committedFrames == 2);
    CHECK(readbackSnapshot.discardedCandidates == 1);
    CHECK(readbackSnapshot.readbackBytes == 3 * first.pixels.size());
    CHECK(readbackSnapshot.cpuBufferHighWater <= 3);
    CHECK(readbackSnapshot.queueHighWater == 1);
    REQUIRE_FALSE(readbackSnapshot.resetPending);
    const auto currentJson = pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", captureSnapshot, normalizedSnapshot, readbackSnapshot, snapshot, 0, 2);
    CHECK(currentJson.find("\"visual\":{\"domain\":" + newDomainJson + ",\"temporalStateCurrent\":true") != std::string::npos);
    REQUIRE(readback->Stop());
    RequireNoTemporalState(processor->GetSnapshot());
    CHECK_FALSE(processor->GetSnapshot().domain);
    normalizedSnapshot.active = false;
    const auto stoppedJson = pbdecoder::SerializeCaptureBootstrapSnapshot("capture-final", captureSnapshot, normalizedSnapshot, readback->GetSnapshot(), processor->GetSnapshot(), 0, 3);
    CHECK(stoppedJson.find("\"visual\":{\"domain\":null,\"temporalStateCurrent\":false") != std::string::npos);
    graphics.CheckDebug();
}
