// Private synchronous admission tests, not a pixel/FEC/field certification.
#include "local_desktop_runtime.cpp"
#include "support.h"
#include <fstream>
#include <iostream>

namespace pbapp
{
namespace
{
struct DecisionFixture
{
    std::vector<std::byte> raw = std::vector<std::byte>(4000, std::byte{0x53});
    TransferDescription description = DescribeSource(raw, false, 3, pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId);
    std::vector<std::byte> transport;

    DecisionFixture()
    {
        const auto& segment = description.segments.at(0);
        std::array<std::byte, outerBlockBytes> payload{};
        std::uint32_t payloadBytes = 0;
        if (segment.descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
        {
            auto encoder = pbouterfec::WirehairV2Encoder::Recreate(segment.inMemoryEncodedBytes, segment.descriptor);
            RequireResult(encoder, "test encoder creation failed");
            const auto encoded = encoder.Value().EncodeBlock(0, payload);
            RequireResult(encoded, "test equation failed");
            payloadBytes = encoded.Value();
        }
        else
        {
            auto encoder = pbouterfec::DirectRepeatEncoder::Create(segment.inMemoryEncodedBytes, outerBlockBytes);
            RequireResult(encoder, "test repeat creation failed");
            const auto encoded = encoder.Value().EncodeBlock(0, payload);
            RequireResult(encoded, "test repeat block failed");
            payloadBytes = encoded.Value();
        }
        const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData, pbprotocol::kTransportProtocolMinor, 0,
            segment.descriptor.sessionTag, 0, 0, static_cast<std::uint16_t>(payloadBytes)};
        transport.resize(pbprotocol::GetTransportSerializedSize(header));
        RequireResult(pbprotocol::SerializeTransportBlock(header, std::span(payload).first(payloadBytes), transport), "test transport serialization failed");
    }

    [[nodiscard]] std::unique_ptr<pbdemodd3d11::CaptureDemodulatorResult> Frame(const std::uint64_t sequence) const
    {
        auto result = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>();
        result->kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
        result->geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
        result->metadata.domain.sourceId[0] = std::byte{1};
        result->metadata.domain.captureEpoch = 1;
        result->metadata.captureObservation = sequence;
        result->metadata.sourceGeneration = 1;
        result->metadata.slotGeneration = sequence;
        result->metadata.timestamp.monotonic100ns = static_cast<std::int64_t>(sequence * 1000000);
        result->demodulation.metadata = result->metadata;
        result->bootstrap.erasure = pbmodulation::LocalDesktopErasureReason::None;
        result->bootstrap.geometry.scaleX = 1;
        result->bootstrap.geometry.scaleY = 1;
        auto& observation = result->demodulation.unifiedObservation;
        observation.inputValid = true;
        observation.frameErasure = pbmodulation::UnifiedErasureReason::None;
        observation.bootstrap = result->bootstrap;
        observation.bootstrapRecord.protocolVersion = pbprotocol::GetProtocolVersion();
        observation.bootstrapRecord.visualProfileId = description.session.sessionVisualProfileId;
        observation.bootstrapRecord.visualLayoutVersion = pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion;
        observation.bootstrapRecord.sessionTag = pbprotocol::DeriveSessionTag(description.session.sessionId);
        observation.bootstrapRecord.frameSequence = sequence;
        result->demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
        RequireResult(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result->bootstrapRecord), "test bootstrap serialization failed");
        return result;
    }
};

void AddDecisionBlock(pbdemodd3d11::CaptureDemodulatorResult& frame, const std::uint8_t slot, const pbmodulation::UnifiedSlotKind kind, const std::vector<std::byte>& bytes)
{
    auto& demodulation = frame.demodulation;
    Require(demodulation.acceptedUnifiedBlockCount < 15 && bytes.size() <= pbmodulation::kUnifiedInformationBytes, "test handoff capacity");
    auto& block = demodulation.acceptedUnifiedBlocks[demodulation.acceptedUnifiedBlockCount++];
    block.codewordSlot = slot;
    block.kind = kind;
    block.size = static_cast<std::uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), block.bytes.begin());
}

struct DecisionHarness
{
    pbprotocol::ReceiverResourcePolicy policy;
    pbreceiver::ReceiverIngress receiver;
    SnapshotStore<DecoderSnapshot> snapshot;
    AuthoritativeCompletion completion;
    ReceiverPipeline pipeline;
    ReceiverDecisionTrace trace;
    bool enabled;

    DecisionHarness(const std::filesystem::path& root, const bool enabledValue, const pbprotocol::ReceiverResourcePolicy& selectedPolicy = MakeUnifiedReceiverResourcePolicy())
        : policy(selectedPolicy), receiver(std::move(pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes)).Value()),
          pipeline(receiver, root.wstring(), policy, snapshot, completion, 1, std::chrono::steady_clock::now(), VisualProfile::UnifiedLc4, false, false, true), enabled(enabledValue)
    {
        Require(std::filesystem::create_directory(root), "test directory already exists");
        snapshot.Update([](DecoderSnapshot& value)
        {
            value.runGeneration = 1;
            value.visualProfile = VisualProfile::UnifiedLc4;
        });
    }
    [[nodiscard]] ReceiverProcessResult Run(const pbdemodd3d11::CaptureDemodulatorResult& frame)
    {
        return pipeline.Process(frame, enabled ? &trace : nullptr);
    }
    void MustFail(const pbdemodd3d11::CaptureDemodulatorResult& frame)
    {
        bool failed = false;
        try
        {
            static_cast<void>(Run(frame));
        }
        catch (const RuntimeFailure&)
        {
            failed = true;
        }
        Require(failed && !completion.published, "expected fail-closed rejection");
        if (enabled)
        {
            Require(trace.reason == ReceiverFrameReason::ProcessingFailed, "missing failed-frame diagnostic");
        }
    }
};

void TestMissingSession(const std::filesystem::path& root, const DecisionFixture& fixture, const bool enabled)
{
    DecisionHarness harness(root, enabled);
    auto frame = fixture.Frame(1);
    AddDecisionBlock(*frame, 0, pbmodulation::UnifiedSlotKind::Control, fixture.description.segments[0].control);
    AddDecisionBlock(*frame, 3, pbmodulation::UnifiedSlotKind::Transport, fixture.transport);
    const auto result = harness.Run(*frame);
    Require(!result.carrierAccepted && !result.uniqueAdmission && harness.receiver.GetTelemetry().activeOuterFecDecoderCount == 0,
        "missing Session must not admit data or allocate a decoder");
    Require(std::filesystem::is_empty(root), "missing Session created output state");
    if (enabled)
    {
        Require(!harness.trace.before.sessionReady && !harness.trace.after.sessionReady, "invented session binding");
        Require(harness.trace.slots[0].reason == ReceiverSlotReason::ControlUnknownSession && harness.trace.slots[0].receiverCalled, "missing Control UnknownSession reason");
        Require(harness.trace.slots[3].reason == ReceiverSlotReason::WaitingForSession && !harness.trace.slots[3].receiverCalled &&
            !harness.trace.slots[3].dataDisposition, "uncalled data must not have a disposition");
    }
}

void TestBindAndDuplicates(const std::filesystem::path& root, const DecisionFixture& fixture, const bool enabled)
{
    DecisionHarness harness(root, enabled);
    auto frame = fixture.Frame(1);
    AddDecisionBlock(*frame, 3, pbmodulation::UnifiedSlotKind::Transport, fixture.transport);
    AddDecisionBlock(*frame, 0, pbmodulation::UnifiedSlotKind::Control, fixture.description.segments[0].control);
    AddDecisionBlock(*frame, 1, pbmodulation::UnifiedSlotKind::Control, fixture.description.sessionControl);
    Require(harness.Run(*frame).uniqueAdmission, "same-frame Session must bind before Segment and Data");
    if (enabled)
    {
        Require(!harness.trace.before.sessionReady && harness.trace.after.sessionReady, "binding transition missing");
        Require(harness.trace.slots[3].dataDisposition == pbreceiver::ReceiverDataDisposition::AcceptedNeedMore &&
            harness.trace.slots[3].outerSymbolAdmission == pbreceiver::ReceiverOuterSymbolAdmission::Unique, "actual admission not recorded");
    }
    frame->metadata.captureObservation = 2;
    frame->metadata.timestamp.monotonic100ns += 1000000;
    frame->demodulation.metadata = frame->metadata;
    const auto duplicate = harness.Run(*frame);
    Require(!duplicate.carrierAccepted && !duplicate.uniqueAdmission, "same-frame duplicate was readmitted");
    if (enabled)
    {
        Require(harness.trace.slots[3].reason == ReceiverSlotReason::AlreadyAdmitted && !harness.trace.slots[3].receiverCalled &&
            !harness.trace.slots[3].dataDisposition, "cache skip reported as a fresh Receiver return");
    }
    frame = fixture.Frame(3);
    AddDecisionBlock(*frame, 3, pbmodulation::UnifiedSlotKind::Transport, fixture.transport);
    const auto repeated = harness.Run(*frame);
    Require(repeated.carrierAccepted && !repeated.uniqueAdmission, "identical equation classified as unique");
    if (enabled)
    {
        Require(harness.trace.slots[3].receiverCalled && harness.trace.slots[3].outerSymbolAdmission == pbreceiver::ReceiverOuterSymbolAdmission::IdenticalDuplicate,
            "Receiver duplicate must differ from cached slot skip");
    }
}

void TestConflict(const std::filesystem::path& root, const DecisionFixture& fixture, const bool enabled)
{
    DecisionHarness harness(root, enabled);
    auto frame = fixture.Frame(1);
    AddDecisionBlock(*frame, 0, pbmodulation::UnifiedSlotKind::Control, fixture.description.sessionControl);
    Require(harness.Run(*frame).carrierAccepted, "initial session rejected");
    auto conflicting = fixture.description;
    conflicting.session.fileNameUtf8 = "conflicting.bin";
    FinalizeTransferControls(conflicting);
    frame = fixture.Frame(2);
    AddDecisionBlock(*frame, 0, pbmodulation::UnifiedSlotKind::Control, conflicting.sessionControl);
    harness.MustFail(*frame);
    if (enabled)
    {
        Require(harness.trace.slots[0].reason == ReceiverSlotReason::ControlRejected && harness.trace.slots[0].protocolError.has_value(), "conflict reason missing");
    }
}

void TestResource(const std::filesystem::path& root, const DecisionFixture& fixture, const bool enabled)
{
    auto policy = MakeUnifiedReceiverResourcePolicy();
    policy.maxOrphanTransportBlocks = 1;
    DecisionHarness harness(root, enabled, policy);
    auto frame = fixture.Frame(1);
    AddDecisionBlock(*frame, 0, pbmodulation::UnifiedSlotKind::Control, fixture.description.sessionControl);
    AddDecisionBlock(*frame, 3, pbmodulation::UnifiedSlotKind::Transport, fixture.transport);
    Require(harness.Run(*frame).uniqueAdmission, "bounded first orphan not admitted");
    if (enabled)
    {
        Require(harness.trace.slots[3].dataDisposition == pbreceiver::ReceiverDataDisposition::CachedOrphan, "orphan mislabeled as bound useful equation");
    }
    auto payload = fixture.transport;
    const auto parsed = pbprotocol::ParseTransportBlock(payload);
    RequireResult(parsed, "test transport parse");
    auto header = parsed.Value().header;
    header.outerBlockId = 1;
    std::vector<std::byte> second(payload.size());
    RequireResult(pbprotocol::SerializeTransportBlock(header, parsed.Value().payload, second), "second orphan serialization");
    frame = fixture.Frame(2);
    AddDecisionBlock(*frame, 3, pbmodulation::UnifiedSlotKind::Transport, second);
    Require(!harness.Run(*frame).carrierAccepted, "orphan quota was bypassed");
    if (enabled)
    {
        Require(harness.trace.slots[3].reason == ReceiverSlotReason::TransportResourceRejected && harness.trace.slots[3].resourceRejected &&
            harness.trace.slots[3].receiverReturned && !harness.trace.slots[3].dataDisposition, "resource rejection fabricated a data result");
    }
}

void TestErasureAndBounds(const std::filesystem::path& root, const DecisionFixture& fixture, const bool enabled)
{
    DecisionHarness harness(root, enabled);
    auto frame = fixture.Frame(1);
    frame->bootstrap.erasure = pbmodulation::LocalDesktopErasureReason::InvalidGeometry;
    Require(!harness.Run(*frame).carrierAccepted, "erasure accepted");
    if (enabled)
    {
        Require(harness.trace.reason == ReceiverFrameReason::BootstrapRejected && !harness.trace.slots[0].present, "erasure invented a slot call");
    }
    frame = fixture.Frame(2);
    AddDecisionBlock(*frame, 255, pbmodulation::UnifiedSlotKind::Transport, fixture.transport);
    harness.MustFail(*frame);
    Require(std::filesystem::is_empty(root), "invalid handoff mutated output state");
}

void TestTraceBounds()
{
    static_assert(sizeof(ReceiverDecisionTrace) < 4096);
    Require(CanAppendRecordedTrace(0, 65536) && !CanAppendRecordedTrace(0, 65537), "record cap changed");
    constexpr std::uint64_t maximum = 64ULL * 1024 * 1024;
    Require(CanAppendRecordedTrace(maximum - 1, 1) && !CanAppendRecordedTrace(maximum, 1) &&
        !CanAppendRecordedTrace(UINT64_MAX, 1) && !CanAppendRecordedTrace(1, UINT64_MAX), "trace cap overflow");
    ReceiverDecisionTrace trace;
    for (auto& slot : trace.slots)
    {
        slot.present = true;
        slot.segmentOrdinal = UINT64_MAX;
        slot.outerBlockId = UINT32_MAX;
    }
    std::ostringstream output;
    WriteReceiverDecisionTrace(output, trace);
    Require(output.str().size() < 12000 && output.str().find("\"dataDisposition\":null") != std::string::npos, "fixed trace size or null semantics failed");
    output.setstate(std::ios::badbit);
    WriteReceiverDecisionTrace(output, trace);
    Require(!output.good(), "diagnostic stream error was cleared");
}
}
}

int wmain(const int count, wchar_t** const arguments)
{
    try
    {
        pbapp::Require(count == 2, "new test root required");
        const auto root = std::filesystem::absolute(arguments[1]);
        pbstep3b::RequireLocal(root);
        pbapp::Require(std::filesystem::create_directory(root), "new test root required");
        const pbapp::DecisionFixture fixture;
        for (const bool enabled : {false, true})
        {
            const std::string suffix = enabled ? "-on" : "-off";
            pbapp::TestMissingSession(root / ("missing" + suffix), fixture, enabled);
            pbapp::TestBindAndDuplicates(root / ("bind-duplicates" + suffix), fixture, enabled);
            pbapp::TestConflict(root / ("conflict" + suffix), fixture, enabled);
            pbapp::TestResource(root / ("resource-orphan" + suffix), fixture, enabled);
            pbapp::TestErasureAndBounds(root / ("erasure-bounds" + suffix), fixture, enabled);
            std::cout << "PASS 5 admission cases diagnostics=" << enabled << '\n';
        }
        pbapp::TestTraceBounds();
        std::cout << "PASS 6 test families; 10 admission cases plus trace boundaries; pixel/WARP observations=0\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
