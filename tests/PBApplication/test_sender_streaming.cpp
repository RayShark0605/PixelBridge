#include "local_desktop_runtime.h"
#include "sender_carousel_scheduler.h"
#include "encoder_session_store.h"

#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/protocol_types.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace
{

class ScratchDirectory
{
public:
    explicit ScratchDirectory(const wchar_t* name)
    {
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name;
        std::error_code error;
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

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> WriteDeterministicSource(
    const std::filesystem::path& path, const std::uint64_t byteCount)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    constexpr std::size_t chunkBytes = 1024 * 1024;
    std::vector<std::byte> chunk(chunkBytes);
    pbprotocol::Blake3Hasher hasher;
    std::uint64_t state = 0xD1B54A32D192ED03ULL;
    std::uint64_t writtenBytes = 0;
    while (writtenBytes < byteCount)
    {
        const std::size_t currentBytes = static_cast<std::size_t>((std::min)(
            byteCount - writtenBytes, static_cast<std::uint64_t>(chunk.size())));
        for (std::size_t offset = 0; offset < currentBytes; offset += sizeof(std::uint64_t))
        {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            const std::uint64_t randomValue = state * 0x2545F4914F6CDD1DULL;
            const std::size_t availableBytes = (std::min)(sizeof(randomValue), currentBytes - offset);
            for (std::size_t byteIndex = 0; byteIndex < availableBytes; byteIndex++)
            {
                chunk[offset + byteIndex] = static_cast<std::byte>(randomValue >> (byteIndex * 8U));
            }
        }
        const std::span<const std::byte> currentChunk(chunk.data(), currentBytes);
        hasher.Update(currentChunk);
        output.write(reinterpret_cast<const char*>(currentChunk.data()),
            static_cast<std::streamsize>(currentChunk.size()));
        REQUIRE(output.good());
        writtenBytes += currentBytes;
    }
    output.flush();
    REQUIRE(output.good());
    return hasher.Finalize();
}

[[nodiscard]] std::string BytesToHex(const std::span<const std::byte> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(bytes[index]);
        text[index * 2] = digits[byte >> 4U];
        text[index * 2 + 1] = digits[byte & 0x0FU];
    }
    return text;
}

void WriteHeadlessReport(const pbapp::EncoderStreamingCarouselProbeSnapshot& snapshot)
{
    const std::filesystem::path reportPath = std::filesystem::path(PB_TEST_SCRATCH_ROOT) /
        L"g02-sender-headless-report.json";
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    REQUIRE(report);
    report << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"gate\": \"G02\",\n"
           << "  \"mode\": \"headless-no-data-window\",\n"
           << "  \"sourceBytes\": " << snapshot.sourceBytes << ",\n"
           << "  \"segmentCount\": " << snapshot.segmentCount << ",\n"
           << "  \"sessionId\": \"" << BytesToHex(snapshot.sessionId.bytes) << "\",\n"
           << "  \"wholeFileDigest\": \"" << BytesToHex(snapshot.wholeFileDigest) << "\",\n"
           << "  \"completedCarouselPasses\": " << snapshot.completedCarouselPasses << ",\n"
           << "  \"scheduledFrames\": " << snapshot.scheduledFrames << ",\n"
           << "  \"controlFrames\": " << snapshot.controlFrames << ",\n"
           << "  \"dataFrames\": " << snapshot.dataFrames << ",\n"
           << "  \"scheduledSystematicEquations\": " << snapshot.scheduledSystematicEquations << ",\n"
           << "  \"scheduledRepairEquations\": " << snapshot.scheduledRepairEquations << ",\n"
           << "  \"paddingDuplicateSlots\": " << snapshot.paddingDuplicateSlots << ",\n"
           << "  \"descriptorResidentEncodedBytes\": " << snapshot.descriptorResidentEncodedBytes << ",\n"
           << "  \"peakResidentEncodedSegmentCount\": " << snapshot.peakResidentEncodedSegmentCount << ",\n"
           << "  \"peakResidentEncodedSegmentBytes\": " << snapshot.peakResidentEncodedSegmentBytes << ",\n"
           << "  \"initialFrameSequence\": " << snapshot.initialFrameSequence << ",\n"
           << "  \"lastUsedFrameSequence\": " << snapshot.lastUsedFrameSequence << ",\n"
           << "  \"persistedFrameSequenceLeaseEnd\": " << snapshot.persistedFrameSequenceLeaseEnd << ",\n"
           << "  \"restartedFrameSequenceStart\": " << snapshot.restartedFrameSequenceStart << ",\n"
           << "  \"sourceWriteShareDenied\": " << (snapshot.sourceWriteShareDenied ? "true" : "false") << ",\n"
           << "  \"sourceDeleteShareDenied\": " << (snapshot.sourceDeleteShareDenied ? "true" : "false") << ",\n"
           << "  \"restartWasResumed\": " << (snapshot.restartWasResumed ? "true" : "false") << ",\n"
           << "  \"equationReproducible\": " << (snapshot.equationReproducible ? "true" : "false") << ",\n"
           << "  \"segments\": [\n";
    for (std::size_t index = 0; index < snapshot.segments.size(); index++)
    {
        const pbapp::EncoderStreamingSegmentProbeSnapshot& segment = snapshot.segments[index];
        report << "    {\"segmentOrdinal\": " << segment.segmentOrdinal
               << ", \"rawBytes\": " << segment.rawBytes
               << ", \"encodedBytes\": " << segment.encodedBytes
               << ", \"systematicBlockCount\": " << segment.systematicBlockCount
               << ", \"repairEquationsPerRound\": " << segment.repairEquationsPerRound
               << ", \"paddingDuplicateSlotsPerRound\": " << segment.paddingDuplicateSlotsPerRound
               << ", \"firstRepairIdPass0\": " << segment.firstRepairIdPass0
               << ", \"firstRepairIdPass1\": " << segment.firstRepairIdPass1
               << ", \"persistedRepairLeaseEnd\": " << segment.persistedRepairLeaseEnd
               << ", \"restartedRepairIdStart\": " << segment.restartedRepairIdStart << "}"
               << (index + 1 == snapshot.segments.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    report.flush();
    REQUIRE(report.good());
}

} // namespace

TEST_CASE("Sender Carousel scheduler separates exact equations from physical padding and repeats Control by time",
    "[application][encoder][sender][scheduler]")
{
    pbapp::SenderCarouselSchedulerConfig config;
    config.systematicBlockCount = 3;
    config.dataSlotsPerFrame = 4;
    config.controlRepetitions = 1;
    config.wirehair = true;
    pbapp::SenderCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderCarouselScheduler::Create(config, scheduler));
    const pbapp::SenderCarouselRoundSnapshot compact = scheduler.GetSnapshot();
    REQUIRE(compact.systematicEquationCount == 3);
    REQUIRE(compact.repairEquationCount == 16);
    REQUIRE(compact.scheduledEquationCount == 19);
    REQUIRE(compact.dataFrameCount == 5);
    REQUIRE(compact.paddingDuplicateSlotCount == 1);
    REQUIRE(compact.controlBurstCount == 1);
    REQUIRE(compact.totalFrameCount == 8);

    std::array<std::uint32_t, 5> expectedEquationCounts{4, 4, 4, 4, 3};
    std::size_t dataIndex = 0;
    std::array<pbapp::SenderScheduledFrameKind, 3> expectedControls{
        pbapp::SenderScheduledFrameKind::SessionControl,
        pbapp::SenderScheduledFrameKind::ManifestControl,
        pbapp::SenderScheduledFrameKind::SegmentControl};
    std::size_t controlIndex = 0;
    while (!scheduler.IsComplete())
    {
        pbapp::SenderScheduledFrame frame;
        REQUIRE(scheduler.GetCurrentFrame(frame));
        if (frame.kind == pbapp::SenderScheduledFrameKind::Data)
        {
            REQUIRE(dataIndex < expectedEquationCounts.size());
            REQUIRE(frame.scheduledEquationCount == expectedEquationCounts[dataIndex]);
            dataIndex++;
        }
        else
        {
            REQUIRE(controlIndex < expectedControls.size());
            REQUIRE(frame.kind == expectedControls[controlIndex]);
            controlIndex++;
        }
        REQUIRE(scheduler.Advance());
    }
    REQUIRE(dataIndex == expectedEquationCounts.size());
    REQUIRE(controlIndex == expectedControls.size());

    config.systematicBlockCount = 6400;
    config.logicalFramesPerSecond = 5;
    REQUIRE(pbapp::SenderCarouselScheduler::Create(config, scheduler));
    const pbapp::SenderCarouselRoundSnapshot longRound = scheduler.GetSnapshot();
    REQUIRE(longRound.repairEquationCount == 1280);
    REQUIRE(longRound.scheduledEquationCount == 7680);
    REQUIRE(longRound.dataFrameCount == 1920);
    REQUIRE(longRound.controlBurstCount == 41);
    REQUIRE(longRound.totalFrameCount == 2043);
    REQUIRE(longRound.paddingDuplicateSlotCount == 0);
    std::array<std::uint64_t, 4> kindCounts{};
    std::uint64_t dataFramesSinceControlBurst = 0;
    std::uint64_t schedulerFrameIndex = 0;
    std::uint64_t previousSessionFrameIndex = 0;
    bool hasPreviousSessionFrame = false;
    while (!scheduler.IsComplete())
    {
        pbapp::SenderScheduledFrame frame;
        REQUIRE(scheduler.GetCurrentFrame(frame));
        const std::size_t kindIndex = static_cast<std::size_t>(frame.kind);
        REQUIRE(kindIndex < kindCounts.size());
        kindCounts[kindIndex]++;
        if (frame.kind == pbapp::SenderScheduledFrameKind::Data)
        {
            dataFramesSinceControlBurst++;
            REQUIRE(dataFramesSinceControlBurst <= 47);
        }
        else if (frame.kind == pbapp::SenderScheduledFrameKind::SessionControl)
        {
            if (hasPreviousSessionFrame)
            {
                REQUIRE(schedulerFrameIndex - previousSessionFrameIndex == 50);
            }
            previousSessionFrameIndex = schedulerFrameIndex;
            hasPreviousSessionFrame = true;
            dataFramesSinceControlBurst = 0;
        }
        REQUIRE(scheduler.Advance());
        schedulerFrameIndex++;
    }
    REQUIRE(kindCounts[static_cast<std::size_t>(pbapp::SenderScheduledFrameKind::SessionControl)] == 41);
    REQUIRE(kindCounts[static_cast<std::size_t>(pbapp::SenderScheduledFrameKind::ManifestControl)] == 41);
    REQUIRE(kindCounts[static_cast<std::size_t>(pbapp::SenderScheduledFrameKind::SegmentControl)] == 41);
    REQUIRE(kindCounts[static_cast<std::size_t>(pbapp::SenderScheduledFrameKind::Data)] == 1920);

    config.systematicBlockCount = 2;
    config.logicalFramesPerSecond = 0;
    config.wirehair = false;
    REQUIRE(pbapp::SenderCarouselScheduler::Create(config, scheduler));
    const pbapp::SenderCarouselRoundSnapshot directRound = scheduler.GetSnapshot();
    REQUIRE(directRound.repairEquationCount == 0);
    REQUIRE(directRound.scheduledEquationCount == 2);
    REQUIRE(directRound.dataFrameCount == 1);
    REQUIRE(directRound.paddingDuplicateSlotCount == 2);
    REQUIRE(directRound.totalFrameCount == 4);

    config.systematicBlockCount = pbapp::senderCarouselMaximumSystematicBlockCount + 1;
    REQUIRE_FALSE(pbapp::SenderCarouselScheduler::Create(config, scheduler));
    REQUIRE(scheduler.IsComplete());
    pbapp::SenderScheduledFrame unavailable;
    REQUIRE_FALSE(scheduler.GetCurrentFrame(unavailable));
}

TEST_CASE("Streaming sender prescans a multi-Segment file and resumes monotonic reproducible Carousel state",
    "[application][encoder][sender][session][persistence][streaming][headless]")
{
    ScratchDirectory scratch(L"g02-streaming-sender");
    const std::filesystem::path sourcePath = scratch.GetPath() / L"source.bin";
    const std::filesystem::path sessionRoot = scratch.GetPath() / L"sessions";
    const std::uint64_t sourceBytes = 3ULL * pbprotocol::kDefaultSourceSegmentTargetBytes + 4096ULL;
    const std::array<std::byte, pbprotocol::kDigestBytes> expectedDigest =
        WriteDeterministicSource(sourcePath, sourceBytes);

    pbapp::EncoderStreamingCarouselProbeSnapshot snapshot;
    const pbapp::RuntimeStatus status = pbapp::EncoderRuntimeTestAccess::ProbeStreamingCarouselFile(
        sourcePath.wstring(), sessionRoot, true, 3, 5, 2, snapshot);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(snapshot.sourceBytes == sourceBytes);
    REQUIRE(snapshot.segmentCount == 4);
    REQUIRE(snapshot.segments.size() == snapshot.segmentCount);
    REQUIRE(snapshot.wholeFileDigest == expectedDigest);
    REQUIRE(snapshot.completedCarouselPasses == 2);
    REQUIRE(snapshot.scheduledFrames == snapshot.controlFrames + snapshot.dataFrames);
    REQUIRE(snapshot.controlFrames > snapshot.segmentCount * snapshot.completedCarouselPasses * 3);
    REQUIRE(snapshot.dataFrames > 0);
    REQUIRE(snapshot.scheduledSystematicEquations > 0);
    REQUIRE(snapshot.scheduledRepairEquations > 0);
    REQUIRE(snapshot.paddingDuplicateSlots > 0);
    REQUIRE(snapshot.descriptorResidentEncodedBytes == 0);
    REQUIRE(snapshot.peakResidentEncodedSegmentCount == 2);
    REQUIRE(snapshot.peakResidentEncodedSegmentBytes <=
        2ULL * pbprotocol::kDefaultSourceSegmentTargetBytes);
    REQUIRE(snapshot.initialFrameSequence == 0);
    REQUIRE(snapshot.persistedFrameSequenceLeaseEnd % pbapp::encoderDurableIdLeaseSize == 0);
    REQUIRE(snapshot.restartedFrameSequenceStart == snapshot.persistedFrameSequenceLeaseEnd);
    REQUIRE(snapshot.restartedFrameSequenceStart > snapshot.lastUsedFrameSequence);
    REQUIRE(snapshot.sourceWriteShareDenied);
    REQUIRE(snapshot.sourceDeleteShareDenied);
    REQUIRE(snapshot.restartWasResumed);
    REQUIRE(snapshot.equationReproducible);

    std::uint64_t expectedSystematicEquations = 0;
    std::uint64_t expectedRepairEquations = 0;
    for (const pbapp::EncoderStreamingSegmentProbeSnapshot& segment : snapshot.segments)
    {
        REQUIRE(segment.compressionCodec == pbprotocol::CompressionCodec::Raw);
        REQUIRE(segment.outerFecMode == pbprotocol::OuterFecMode::WirehairV2);
        REQUIRE(segment.rawBytes == segment.encodedBytes);
        REQUIRE(segment.systematicBlockCount >= pbouterfec::kWirehairV2MinimumBlockCount);
        REQUIRE(segment.systematicBlockCount <= pbouterfec::kWirehairV2MaximumBlockCount);
        REQUIRE(segment.repairEquationsPerRound == (std::max)(16ULL,
            (static_cast<std::uint64_t>(segment.systematicBlockCount) + 4ULL) / 5ULL));
        REQUIRE(segment.firstRepairIdPass0 == segment.systematicBlockCount);
        REQUIRE(segment.firstRepairIdPass1 == static_cast<std::uint64_t>(segment.firstRepairIdPass0) +
            segment.repairEquationsPerRound);
        REQUIRE(segment.persistedRepairLeaseEnd % pbapp::encoderDurableIdLeaseSize == 0);
        REQUIRE(segment.restartedRepairIdStart == segment.persistedRepairLeaseEnd);
        REQUIRE(segment.restartedRepairIdStart > segment.firstRepairIdPass1);
        expectedSystematicEquations += 2ULL * segment.systematicBlockCount;
        expectedRepairEquations += 2ULL * segment.repairEquationsPerRound;
    }
    REQUIRE(snapshot.scheduledSystematicEquations == expectedSystematicEquations);
    REQUIRE(snapshot.scheduledRepairEquations == expectedRepairEquations);
    WriteHeadlessReport(snapshot);
}
