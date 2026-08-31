#include "temporal_corpus_core.h"

#include "application_model.h"
#include "codec_probe_core.h"
#include "pbprotocol/blake3_digest.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbremotevisualtemporalcorpus
{
namespace
{

struct TemporalEventInput
{
    std::string_view name;
    std::span<const std::byte> frame;
    std::uint64_t expectedSegmentOrdinal = 0;
    std::uint64_t captureEpoch = 0;
    std::int64_t timestamp100ns = 0;
    pbapp::VisualIdentityDisposition expectedDisposition = pbapp::VisualIdentityDisposition::Invalid;
    bool expectedAdmissionAttempt = false;
    bool expectedDuplicateRefinement = false;
    std::uint32_t expectedAdmittedBlocks = 0;
};

struct TemporalEventEvidence
{
    std::string name;
    std::uint64_t captureEpoch = 0;
    std::int64_t timestamp100ns = 0;
    std::uint64_t sessionTag = 0;
    std::uint64_t frameSequence = 0;
    pbapp::VisualIdentityDisposition disposition = pbapp::VisualIdentityDisposition::Invalid;
    pbremotevisualcodecprobe::CodecFrameSummary frame;
    bool acceptedCarrier = false;
    bool admissionAttempted = false;
    bool duplicateRefinement = false;
    std::uint32_t admittedBlocks = 0;
    bool expectationMatched = false;
};

struct TemporalScenarioEvidence
{
    std::string name;
    std::vector<TemporalEventEvidence> events;
    pbapp::VisualIdentitySnapshot identity;
    pbapp::RemoteDuplicateRefinementSnapshot refinement;
    bool expectationMatched = false;
};

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendSigned(std::string& output, const std::int64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("signed integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    output.push_back('"');
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
            output.push_back(kHexDigits[character & 0x0FU]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

std::string DigestToHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); index++)
    {
        const unsigned int value = std::to_integer<unsigned int>(digest[index]);
        output[index * 2] = kHexDigits[value >> 4];
        output[index * 2 + 1] = kHexDigits[value & 0x0FU];
    }
    return output;
}

const char* GetDispositionName(const pbapp::VisualIdentityDisposition disposition) noexcept
{
    switch (disposition)
    {
    case pbapp::VisualIdentityDisposition::Invalid:
        return "Invalid";
    case pbapp::VisualIdentityDisposition::Unique:
        return "Unique";
    case pbapp::VisualIdentityDisposition::Duplicate:
        return "Duplicate";
    case pbapp::VisualIdentityDisposition::Reordered:
        return "Reordered";
    }
    return "Unknown";
}

std::vector<std::byte> ConvertNeutralBgraSequenceToGray(
    const pbremotevisualcodecprobe::CodecSourceSequence& source)
{
    if (source.bgraFrames.size() != pbremotevisualcodecprobe::kBgraFrameBytes *
        pbremotevisualcodecprobe::kSourceFrameCount)
    {
        throw std::runtime_error("codec source has an unexpected byte count");
    }
    std::vector<std::byte> gray(pbremotevisualcodecprobe::kGrayFrameBytes *
        pbremotevisualcodecprobe::kSourceFrameCount);
    for (std::size_t index = 0; index < gray.size(); index++)
    {
        const std::size_t sourceOffset = index * 4;
        if (source.bgraFrames[sourceOffset] != source.bgraFrames[sourceOffset + 1] ||
            source.bgraFrames[sourceOffset] != source.bgraFrames[sourceOffset + 2] ||
            source.bgraFrames[sourceOffset + 3] != std::byte{255})
        {
            throw std::runtime_error("codec source is not neutral opaque BGRA");
        }
        gray[index] = source.bgraFrames[sourceOffset];
    }
    return gray;
}

pbremotevisualcodecprobe::CodecFrameSummary EvaluateFrame(const std::span<const std::byte> frame,
    const std::uint64_t expectedSegmentOrdinal)
{
    pbremotevisualcodecprobe::CodecSequenceEvaluation evaluation;
    std::string error;
    if (!pbremotevisualcodecprobe::EvaluateGray8Sequence(frame, evaluation, error, expectedSegmentOrdinal) ||
        evaluation.frames.size() != 1)
    {
        throw std::runtime_error("temporal frame evaluation failed: " + error);
    }
    if (!evaluation.frames[0].bootstrapAccepted)
    {
        throw std::runtime_error("temporal corpus frame did not preserve Bootstrap identity");
    }
    return evaluation.frames[0];
}

TemporalScenarioEvidence RunTemporalScenario(const std::string_view name,
    const std::span<const TemporalEventInput> inputs)
{
    pbapp::VisualIdentityTracker identity;
    pbapp::RemoteDuplicateRefinementGate refinement;
    TemporalScenarioEvidence scenario;
    scenario.name = name;
    scenario.events.reserve(inputs.size());
    scenario.expectationMatched = true;
    for (const TemporalEventInput& input : inputs)
    {
        TemporalEventEvidence event;
        event.name = input.name;
        event.captureEpoch = input.captureEpoch;
        event.timestamp100ns = input.timestamp100ns;
        event.frame = EvaluateFrame(input.frame, input.expectedSegmentOrdinal);
        event.sessionTag = event.frame.sessionTag;
        event.frameSequence = event.frame.frameSequence;
        event.disposition = identity.Observe(event.frameSequence, input.captureEpoch, input.timestamp100ns,
            event.sessionTag);
        event.acceptedCarrier = event.frame.acceptedTransportBlocks != 0;
        if (event.disposition == pbapp::VisualIdentityDisposition::Unique)
        {
            refinement.StartSequence(input.captureEpoch, event.frameSequence);
            event.admissionAttempted = true;
        }
        else if (event.disposition == pbapp::VisualIdentityDisposition::Duplicate)
        {
            event.duplicateRefinement = refinement.ShouldAttemptDuplicate(input.captureEpoch,
                event.frameSequence, event.acceptedCarrier);
            event.admissionAttempted = event.duplicateRefinement;
        }
        if (event.admissionAttempted && event.acceptedCarrier &&
            refinement.MarkAdmission(input.captureEpoch, event.frameSequence, event.duplicateRefinement))
        {
            event.admittedBlocks = event.frame.acceptedTransportBlocks;
        }
        event.expectationMatched = event.disposition == input.expectedDisposition &&
            event.admissionAttempted == input.expectedAdmissionAttempt &&
            event.duplicateRefinement == input.expectedDuplicateRefinement &&
            event.admittedBlocks == input.expectedAdmittedBlocks;
        scenario.expectationMatched = scenario.expectationMatched && event.expectationMatched;
        scenario.events.push_back(std::move(event));
    }
    scenario.identity = identity.GetSnapshot();
    scenario.refinement = refinement.GetSnapshot();
    return scenario;
}

void AppendIdentitySnapshot(std::string& output, const pbapp::VisualIdentitySnapshot& snapshot)
{
    output.append("{\"uniqueFrames\":");
    AppendUnsigned(output, snapshot.uniqueFrames);
    output.append(",\"duplicateFrames\":");
    AppendUnsigned(output, snapshot.duplicateFrames);
    output.append(",\"reorderedFrames\":");
    AppendUnsigned(output, snapshot.reorderedFrames);
    output.append(",\"gapEvents\":");
    AppendUnsigned(output, snapshot.gapEvents);
    output.append(",\"skippedSequences\":");
    AppendUnsigned(output, snapshot.skippedSequences);
    output.push_back('}');
}

void AppendRefinementSnapshot(std::string& output, const pbapp::RemoteDuplicateRefinementSnapshot& snapshot)
{
    output.append("{\"attempts\":");
    AppendUnsigned(output, snapshot.attempts);
    output.append(",\"recoveries\":");
    AppendUnsigned(output, snapshot.recoveries);
    output.append(",\"currentSequenceAdmitted\":");
    AppendBoolean(output, snapshot.currentSequenceAdmitted);
    output.push_back('}');
}

void AppendScenario(std::string& output, const TemporalScenarioEvidence& scenario)
{
    output.append("{\"name\":");
    AppendJsonString(output, scenario.name);
    output.append(",\"events\":[");
    for (std::size_t index = 0; index < scenario.events.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const TemporalEventEvidence& event = scenario.events[index];
        output.append("{\"name\":");
        AppendJsonString(output, event.name);
        output.append(",\"captureEpoch\":");
        AppendUnsigned(output, event.captureEpoch);
        output.append(",\"timestamp100ns\":");
        AppendSigned(output, event.timestamp100ns);
        output.append(",\"sessionTag\":");
        AppendUnsigned(output, event.sessionTag);
        output.append(",\"frameSequence\":");
        AppendUnsigned(output, event.frameSequence);
        output.append(",\"disposition\":");
        AppendJsonString(output, GetDispositionName(event.disposition));
        output.append(",\"codecClassification\":");
        AppendJsonString(output, event.frame.classification);
        output.append(",\"fecFailures\":");
        AppendUnsigned(output, event.frame.fecFailures);
        output.append(",\"crcFailures\":");
        AppendUnsigned(output, event.frame.crcFailures);
        output.append(",\"identityFailures\":");
        AppendUnsigned(output, event.frame.identityFailures);
        output.append(",\"diagnosticNonTruthCodewords\":");
        AppendUnsigned(output, event.frame.diagnosticFalseCandidates);
        output.append(",\"productionFalseAcceptedCodewords\":");
        AppendUnsigned(output, event.frame.falseAcceptedCodewords);
        output.append(",\"acceptedCarrier\":");
        AppendBoolean(output, event.acceptedCarrier);
        output.append(",\"admissionAttempted\":");
        AppendBoolean(output, event.admissionAttempted);
        output.append(",\"duplicateRefinement\":");
        AppendBoolean(output, event.duplicateRefinement);
        output.append(",\"admittedTransportBlocks\":");
        AppendUnsigned(output, event.admittedBlocks);
        output.append(",\"expectationMatched\":");
        AppendBoolean(output, event.expectationMatched);
        output.push_back('}');
    }
    output.append("],\"identitySnapshot\":");
    AppendIdentitySnapshot(output, scenario.identity);
    output.append(",\"duplicateRefinementSnapshot\":");
    AppendRefinementSnapshot(output, scenario.refinement);
    output.append(",\"expectationMatched\":");
    AppendBoolean(output, scenario.expectationMatched);
    output.push_back('}');
}

void AppendStallInterval(std::string& output, const pbapp::StallIntervalSnapshot& interval)
{
    output.append("{\"count\":");
    AppendUnsigned(output, interval.count);
    output.append(",\"totalMilliseconds\":");
    AppendUnsigned(output, interval.totalMilliseconds);
    output.append(",\"maximumMilliseconds\":");
    AppendUnsigned(output, interval.maximumMilliseconds);
    output.append(",\"currentMilliseconds\":");
    AppendUnsigned(output, interval.currentMilliseconds);
    output.append(",\"active\":");
    AppendBoolean(output, interval.active);
    output.push_back('}');
}

std::string SerializePayload(const std::span<const TemporalScenarioEvidence> scenarios,
    const pbapp::ChannelStallSnapshot& stalls, const TemporalCorpusReport& summary)
{
    std::string output;
    output.reserve(24 * 1024);
    output.append("{\"profile\":\"PB-RemoteVisual-LF4-X1\",\"scenarios\":[");
    for (std::size_t index = 0; index < scenarios.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        AppendScenario(output, scenarios[index]);
    }
    output.append("],\"stallScenario\":{\"capture\":");
    AppendStallInterval(output, stalls.capture);
    output.append(",\"visual\":");
    AppendStallInterval(output, stalls.visual);
    output.append("},\"summary\":{\"scenarioCount\":");
    AppendUnsigned(output, scenarios.size());
    output.append(",\"eventCount\":");
    AppendUnsigned(output, summary.eventCount);
    output.append(",\"admittedTransportBlocks\":");
    AppendUnsigned(output, summary.admittedTransportBlocks);
    output.append(",\"suppressedDuplicateEvents\":");
    AppendUnsigned(output, summary.suppressedDuplicateEvents);
    output.append(",\"suppressedReorderedEvents\":");
    AppendUnsigned(output, summary.suppressedReorderedEvents);
    output.append(",\"duplicateRefinementRecoveries\":");
    AppendUnsigned(output, summary.duplicateRefinementRecoveries);
    output.append(",\"wrongIdentityAcceptedTransportBlocks\":");
    AppendUnsigned(output, summary.wrongIdentityAcceptedTransportBlocks);
    output.append(",\"wrongIdentityDiagnosticCandidates\":");
    AppendUnsigned(output, summary.wrongIdentityDiagnosticCandidates);
    output.append(",\"productionAdmissionSafe\":");
    AppendBoolean(output, summary.productionAdmissionSafe);
    output.append(",\"expectationsMatched\":");
    AppendBoolean(output, summary.expectationsMatched);
    output.append("}}");
    return output;
}

std::string SealPayload(const std::string& payload,
    std::array<std::byte, kTemporalCorpusDigestBytes>& digest)
{
    const auto payloadBytes = std::as_bytes(std::span(payload));
    digest = pbprotocol::ComputeBlake3Digest(payloadBytes);
    std::string output;
    output.reserve(payload.size() + 160);
    output.append("{\"schema\":");
    AppendJsonString(output, kTemporalCorpusSchema);
    output.append(",\"version\":");
    AppendUnsigned(output, kTemporalCorpusVersion);
    output.append(",\"payloadBlake3\":");
    AppendJsonString(output, DigestToHex(digest));
    output.append(",\"payload\":");
    output.append(payload);
    output.append("}\n");
    return output;
}

} // namespace

bool BuildDefaultTemporalCorpus(TemporalCorpusReport& output, std::string& error)
{
    error.clear();
    try
    {
        pbremotevisualcodecprobe::CodecSourceSequence source;
        std::string codecError;
        if (!pbremotevisualcodecprobe::BuildCanonicalSourceSequence(source, codecError))
        {
            throw std::runtime_error("cannot build temporal source: " + codecError);
        }
        const std::vector<std::byte> cleanFrames = ConvertNeutralBgraSequenceToGray(source);
        const auto CleanFrame = [&cleanFrames](const std::size_t index)
        {
            return std::span(cleanFrames).subspan(index * pbremotevisualcodecprobe::kGrayFrameBytes,
                pbremotevisualcodecprobe::kGrayFrameBytes);
        };
        std::vector<std::byte> transportErasure;
        if (!pbremotevisualcodecprobe::BuildAdversarialGrayFrame(
            pbremotevisualcodecprobe::CodecAdversarialFrameKind::TransportErasure, transportErasure, codecError))
        {
            throw std::runtime_error("cannot build transport erasure: " + codecError);
        }
        std::vector<std::byte> wrongIdentity;
        if (!pbremotevisualcodecprobe::BuildAdversarialGrayFrame(
            pbremotevisualcodecprobe::CodecAdversarialFrameKind::ValidCrcWrongIdentity, wrongIdentity, codecError))
        {
            throw std::runtime_error("cannot build wrong-identity frame: " + codecError);
        }

        const std::array orderInputs{
            TemporalEventInput{"unique-0", CleanFrame(0), 0, 1, 0,
                pbapp::VisualIdentityDisposition::Unique, true, false, 4},
            TemporalEventInput{"duplicate-0", CleanFrame(0), 0, 1, 1000000,
                pbapp::VisualIdentityDisposition::Duplicate, false, false, 0},
            TemporalEventInput{"gap-to-2", CleanFrame(2), 2, 1, 2000000,
                pbapp::VisualIdentityDisposition::Unique, true, false, 4},
            TemporalEventInput{"reordered-1", CleanFrame(1), 1, 1, 3000000,
                pbapp::VisualIdentityDisposition::Reordered, false, false, 0},
            TemporalEventInput{"epoch-reset-1", CleanFrame(1), 1, 2, 4000000,
                pbapp::VisualIdentityDisposition::Unique, true, false, 4},
            TemporalEventInput{"epoch-duplicate-1", CleanFrame(1), 1, 2, 5000000,
                pbapp::VisualIdentityDisposition::Duplicate, false, false, 0},
            TemporalEventInput{"invalid-zero-epoch", CleanFrame(2), 2, 0, 6000000,
                pbapp::VisualIdentityDisposition::Invalid, false, false, 0}};
        const std::array refinementInputs{
            TemporalEventInput{"unique-erasure-0", transportErasure, 0, 7, 0,
                pbapp::VisualIdentityDisposition::Unique, true, false, 0},
            TemporalEventInput{"duplicate-clean-refinement-0", CleanFrame(0), 0, 7, 1000000,
                pbapp::VisualIdentityDisposition::Duplicate, true, true, 4},
            TemporalEventInput{"duplicate-after-admission-0", CleanFrame(0), 0, 7, 2000000,
                pbapp::VisualIdentityDisposition::Duplicate, false, false, 0}};
        const std::array wrongIdentityInputs{
            TemporalEventInput{"valid-crc-wrong-identity", wrongIdentity, 0, 9, 0,
                pbapp::VisualIdentityDisposition::Unique, true, false, 0}};
        std::array scenarios{
            RunTemporalScenario("duplicate-gap-reorder-epoch", orderInputs),
            RunTemporalScenario("duplicate-refinement-after-erasure", refinementInputs),
            RunTemporalScenario("valid-crc-wrong-identity", wrongIdentityInputs)};

        const bool orderSnapshotMatches = scenarios[0].identity.uniqueFrames == 3 &&
            scenarios[0].identity.duplicateFrames == 2 && scenarios[0].identity.reorderedFrames == 1 &&
            scenarios[0].identity.gapEvents == 1 && scenarios[0].identity.skippedSequences == 1;
        const bool refinementSnapshotMatches = scenarios[1].identity.uniqueFrames == 1 &&
            scenarios[1].identity.duplicateFrames == 2 && scenarios[1].refinement.attempts == 1 &&
            scenarios[1].refinement.recoveries == 1 && scenarios[1].refinement.currentSequenceAdmitted;

        pbapp::ChannelStallTracker stallTracker;
        stallTracker.Observe(0, 0, 0);
        stallTracker.Observe(500, 1, 1);
        stallTracker.Observe(1000, 2, 1);
        stallTracker.Observe(1500, 3, 1);
        stallTracker.Observe(1800, 4, 2);
        stallTracker.Observe(2000, 4, 2);
        stallTracker.Observe(2800, 4, 2);
        stallTracker.Observe(3200, 5, 3);
        const pbapp::ChannelStallSnapshot stalls = stallTracker.GetSnapshot();
        const bool stallsMatch = stalls.capture.count == 1 && stalls.capture.totalMilliseconds == 1400 &&
            stalls.capture.maximumMilliseconds == 1400 && !stalls.capture.active &&
            stalls.visual.count == 1 && stalls.visual.totalMilliseconds == 1300 &&
            stalls.visual.maximumMilliseconds == 1300 && !stalls.visual.active;

        TemporalCorpusReport report;
        for (const TemporalScenarioEvidence& scenario : scenarios)
        {
            report.eventCount += static_cast<std::uint32_t>(scenario.events.size());
            for (const TemporalEventEvidence& event : scenario.events)
            {
                report.admittedTransportBlocks += event.admittedBlocks;
                if (event.disposition == pbapp::VisualIdentityDisposition::Duplicate && !event.admissionAttempted)
                {
                    report.suppressedDuplicateEvents++;
                }
                if (event.disposition == pbapp::VisualIdentityDisposition::Reordered && !event.admissionAttempted)
                {
                    report.suppressedReorderedEvents++;
                }
            }
        }
        report.duplicateRefinementRecoveries = static_cast<std::uint32_t>(scenarios[1].refinement.recoveries);
        report.wrongIdentityAcceptedTransportBlocks = scenarios[2].events[0].frame.acceptedTransportBlocks;
        report.wrongIdentityDiagnosticCandidates = scenarios[2].events[0].frame.diagnosticFalseCandidates;
        report.productionAdmissionSafe = report.suppressedDuplicateEvents == 3 &&
            report.suppressedReorderedEvents == 1 && report.duplicateRefinementRecoveries == 1 &&
            report.wrongIdentityAcceptedTransportBlocks == 0 && scenarios[2].events[0].admittedBlocks == 0 &&
            scenarios[2].events[0].frame.identityFailures == 4 &&
            report.wrongIdentityDiagnosticCandidates == 4;
        report.expectationsMatched = orderSnapshotMatches && refinementSnapshotMatches && stallsMatch &&
            report.productionAdmissionSafe;
        for (const TemporalScenarioEvidence& scenario : scenarios)
        {
            report.expectationsMatched = report.expectationsMatched && scenario.expectationMatched;
        }
        const std::string payload = SerializePayload(scenarios, stalls, report);
        report.canonicalJson = SealPayload(payload, report.payloadBlake3);
        output = std::move(report);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown temporal corpus generation failure";
    }
    return false;
}

} // namespace pbremotevisualtemporalcorpus
