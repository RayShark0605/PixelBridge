#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace {

using pbprotocol::test::Byte;

class TrackingMemoryResource final : public std::pmr::memory_resource
{
public:
    explicit TrackingMemoryResource(const bool failAllocation = false) noexcept
        : failAllocation_(failAllocation)
    {
    }

    [[nodiscard]] std::size_t BytesInUse() const noexcept
    {
        return bytesInUse_;
    }

    void SetFailAllocation(const bool failAllocation) noexcept
    {
        failAllocation_ = failAllocation;
    }

private:
    [[nodiscard]] void* do_allocate(
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        if (failAllocation_)
        {
            throw std::bad_alloc{};
        }
        void* const allocation = std::pmr::new_delete_resource()->allocate(
            bytes,
            alignment);
        bytesInUse_ += bytes;
        return allocation;
    }

    void do_deallocate(
        void* const allocation,
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        if (bytes > bytesInUse_)
        {
            std::terminate();
        }
        std::pmr::new_delete_resource()->deallocate(
            allocation,
            bytes,
            alignment);
        bytesInUse_ -= bytes;
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }

    bool failAllocation_ = false;
    std::size_t bytesInUse_ = 0;
};

[[nodiscard]] std::vector<std::byte> WrapControlPayload(
    const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence,
    const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        recordType,
        controlSequence,
        sessionTag,
        payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionControlRecord(
    const pbprotocol::SessionDescriptor& descriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 1,
    const pbprotocol::SessionTag* const envelopeTag = nullptr)
{
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        descriptor,
        resourcePolicy,
        payload));
    const pbprotocol::SessionTag sessionTag = envelopeTag == nullptr
        ? pbprotocol::DeriveSessionTag(descriptor.sessionId)
        : *envelopeTag;
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SessionDescriptor,
        controlSequence,
        sessionTag,
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentControlRecord(
    const pbprotocol::SegmentDescriptor& descriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 2)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> payload(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        resourcePolicy,
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SegmentDescriptor,
        controlSequence,
        descriptor.sessionTag,
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeManifestControlRecord(
    const pbprotocol::FinalManifest& finalManifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 3)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::FinalManifest,
        controlSequence,
        pbprotocol::DeriveSessionTag(finalManifest.sessionId),
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeFragmentBytes(
    const std::span<const std::byte> recordBytes,
    const std::uint64_t recordId,
    const std::uint16_t fragmentIndex,
    const std::uint16_t fragmentPayloadBytes = 24)
{
    const auto fragmentResult = pbprotocol::GetControlFragment(
        recordId,
        recordBytes,
        fragmentIndex,
        fragmentPayloadBytes);
    REQUIRE(fragmentResult);
    const auto sizeResult = pbprotocol::GetSerializedSize(
        fragmentResult.Value());
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlFragment(
        fragmentResult.Value(),
        bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::vector<std::byte>> MakeFragments(
    const std::span<const std::byte> recordBytes,
    const std::uint64_t recordId,
    const std::uint16_t fragmentPayloadBytes = 24)
{
    const auto countResult = pbprotocol::GetControlFragmentCount(
        recordBytes,
        fragmentPayloadBytes);
    REQUIRE(countResult);
    std::vector<std::vector<std::byte>> fragments;
    fragments.reserve(countResult.Value());
    for (std::uint16_t fragmentIndex = 0;
         fragmentIndex < countResult.Value();
         fragmentIndex++)
    {
        fragments.push_back(MakeFragmentBytes(
            recordBytes,
            recordId,
            fragmentIndex,
            fragmentPayloadBytes));
    }
    return fragments;
}

[[nodiscard]] pbprotocol::ControlPlaneReceiver MakeReceiver(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        resourcePolicy);
    REQUIRE(receiverResult);
    return std::move(receiverResult).Value();
}

[[nodiscard]] pbprotocol::ProtocolResult<
    pbprotocol::ControlFragmentReceiveResult> ReceiveAllFragments(
    pbprotocol::ControlPlaneReceiver& receiver,
    const std::span<const std::byte> recordBytes,
    const std::uint64_t recordId,
    std::uint64_t& observationOrdinal,
    const std::uint16_t fragmentPayloadBytes = 64)
{
    const auto fragments = MakeFragments(
        recordBytes,
        recordId,
        fragmentPayloadBytes);
    auto result = receiver.ReceiveControlFragment(
        fragments.front(),
        observationOrdinal);
    observationOrdinal++;
    if (fragments.size() > 1U)
    {
        REQUIRE(result);
    }
    for (std::size_t fragmentIndex = 1;
         fragmentIndex < fragments.size();
         fragmentIndex++)
    {
        result = receiver.ReceiveControlFragment(
            fragments[fragmentIndex],
            observationOrdinal);
        observationOrdinal++;
        if (fragmentIndex + 1U < fragments.size())
        {
            REQUIRE(result);
        }
    }
    return result;
}

void RequireInserted(
    const pbprotocol::ProtocolResult<pbprotocol::ControlRecordAdmission>& result,
    const pbprotocol::ControlRecordType recordType,
    const pbprotocol::SessionTag sessionTag)
{
    REQUIRE(result);
    REQUIRE(result.Value().recordType == recordType);
    REQUIRE(result.Value().sessionTag == sessionTag);
    REQUIRE(result.Value().bindDisposition ==
        pbprotocol::DescriptorBindDisposition::Inserted);
}

} // namespace

TEST_CASE("ControlPlaneReceiver is the authoritative typed admission boundary",
          "[pbprotocol][control][receiver][admission]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            117);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);
    const std::vector<std::byte> sessionRecord = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy);
    const std::vector<std::byte> segmentRecord = MakeSegmentControlRecord(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy);
    const std::vector<std::byte> manifestRecord = MakeManifestControlRecord(
        finalManifest,
        sessionDescriptor,
        resourcePolicy);

    SECTION("all three types dispatch once and expose verified state")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        RequireInserted(
            receiver.ReceiveControlRecord(sessionRecord),
            pbprotocol::ControlRecordType::SessionDescriptor,
            sessionTag);
        REQUIRE(receiver.ActiveSessionCount() == 1);
        REQUIRE(receiver.ReservedDescriptorStateBytes() ==
            resourcePolicy.maxDescriptorStateBytes);

        RequireInserted(
            receiver.ReceiveControlRecord(segmentRecord),
            pbprotocol::ControlRecordType::SegmentDescriptor,
            sessionTag);
        const auto segmentCountResult = receiver.BoundSegmentCount(sessionTag);
        REQUIRE(segmentCountResult);
        REQUIRE(segmentCountResult.Value() == 1);

        RequireInserted(
            receiver.ReceiveControlRecord(manifestRecord),
            pbprotocol::ControlRecordType::FinalManifest,
            sessionTag);
        const auto manifestResult = receiver.HasFinalManifest(sessionTag);
        REQUIRE(manifestResult);
        REQUIRE(manifestResult.Value());
        REQUIRE(receiver.ValidateCompleteSegmentMap(sessionTag));

        const auto repeatedSession = receiver.ReceiveControlRecord(sessionRecord);
        REQUIRE(repeatedSession);
        REQUIRE(repeatedSession.Value().bindDisposition ==
            pbprotocol::DescriptorBindDisposition::Repeated);
        const auto repeatedSegment = receiver.ReceiveControlRecord(segmentRecord);
        REQUIRE(repeatedSegment);
        REQUIRE(repeatedSegment.Value().bindDisposition ==
            pbprotocol::DescriptorBindDisposition::Repeated);
        const auto repeatedManifest = receiver.ReceiveControlRecord(manifestRecord);
        REQUIRE(repeatedManifest);
        REQUIRE(repeatedManifest.Value().bindDisposition ==
            pbprotocol::DescriptorBindDisposition::Repeated);
    }

    SECTION("fragmented and complete paths have identical typed outcomes")
    {
        auto completeReceiver = MakeReceiver(resourcePolicy);
        auto fragmentedReceiver = MakeReceiver(resourcePolicy);
        std::uint64_t observationOrdinal = 1;

        const std::array<std::span<const std::byte>, 3> records{
            sessionRecord,
            segmentRecord,
            manifestRecord};
        for (std::size_t recordIndex = 0;
             recordIndex < records.size();
             recordIndex++)
        {
            const auto completeResult =
                completeReceiver.ReceiveControlRecord(records[recordIndex]);
            REQUIRE(completeResult);
            const auto fragmentedResult = ReceiveAllFragments(
                fragmentedReceiver,
                records[recordIndex],
                100U + recordIndex,
                observationOrdinal);
            REQUIRE(fragmentedResult);
            REQUIRE(fragmentedResult.Value().admission.has_value());
            REQUIRE(*fragmentedResult.Value().admission ==
                completeResult.Value());
            REQUIRE(fragmentedResult.Value().disposition ==
                pbprotocol::ControlFragmentReceiveDisposition::
                    DescriptorInserted);
        }

        REQUIRE(fragmentedReceiver.ActiveSessionCount() == 1);
        const auto segmentCountResult =
            fragmentedReceiver.BoundSegmentCount(sessionTag);
        REQUIRE(segmentCountResult);
        REQUIRE(segmentCountResult.Value() == 1);
        const auto manifestResult =
            fragmentedReceiver.HasFinalManifest(sessionTag);
        REQUIRE(manifestResult);
        REQUIRE(manifestResult.Value());
    }

    SECTION("single-fragment records enter the same authoritative path")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const std::vector<std::byte> fragment = MakeFragmentBytes(
            sessionRecord,
            99,
            0,
            static_cast<std::uint16_t>(sessionRecord.size()));
        const auto result = receiver.ReceiveControlFragment(fragment, 1);
        REQUIRE(result);
        REQUIRE(result.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);
        REQUIRE(result.Value().admission.has_value());
        REQUIRE(result.Value().admission->recordType ==
            pbprotocol::ControlRecordType::SessionDescriptor);
        REQUIRE(result.Value().admission->sessionTag == sessionTag);
        REQUIRE(receiver.ActiveSessionCount() == 1);
    }

    SECTION("wrong envelope SessionTag is rejected before registry mutation")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const pbprotocol::SessionTag wrongTag{sessionTag.value ^ 1ULL};
        const std::vector<std::byte> wrongTagRecord = MakeSessionControlRecord(
            sessionDescriptor,
            resourcePolicy,
            10,
            &wrongTag);
        const auto result = receiver.ReceiveControlRecord(wrongTagRecord);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::SessionTagMismatch,
                14});
        REQUIRE(receiver.ActiveSessionCount() == 0);
        REQUIRE(receiver.ReservedDescriptorStateBytes() == 0);
    }

    SECTION("payload Session identity mismatches leave bound state unchanged")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        RequireInserted(
            receiver.ReceiveControlRecord(sessionRecord),
            pbprotocol::ControlRecordType::SessionDescriptor,
            sessionTag);
        const std::uint64_t reservedBytesBefore =
            receiver.ReservedDescriptorStateBytes();

        const auto segmentEnvelopeResult = pbprotocol::ParseControlRecord(
            segmentRecord);
        REQUIRE(segmentEnvelopeResult);
        std::vector<std::byte> wrongSegmentPayload(
            segmentEnvelopeResult.Value().payload.begin(),
            segmentEnvelopeResult.Value().payload.end());
        wrongSegmentPayload[0] ^= Byte(0x01);
        const std::vector<std::byte> wrongSegmentRecord = WrapControlPayload(
            pbprotocol::ControlRecordType::SegmentDescriptor,
            10,
            sessionTag,
            wrongSegmentPayload);
        const auto segmentResult = receiver.ReceiveControlRecord(
            wrongSegmentRecord);
        REQUIRE_FALSE(segmentResult);
        REQUIRE(segmentResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagMismatch);

        const auto manifestEnvelopeResult = pbprotocol::ParseControlRecord(
            manifestRecord);
        REQUIRE(manifestEnvelopeResult);
        std::vector<std::byte> wrongManifestPayload(
            manifestEnvelopeResult.Value().payload.begin(),
            manifestEnvelopeResult.Value().payload.end());
        wrongManifestPayload[0] ^= Byte(0x01);
        const std::vector<std::byte> wrongManifestRecord = WrapControlPayload(
            pbprotocol::ControlRecordType::FinalManifest,
            11,
            sessionTag,
            wrongManifestPayload);
        const auto manifestAdmissionResult = receiver.ReceiveControlRecord(
            wrongManifestRecord);
        REQUIRE_FALSE(manifestAdmissionResult);
        REQUIRE(manifestAdmissionResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionMismatch);

        REQUIRE(receiver.ActiveSessionCount() == 1);
        REQUIRE(receiver.ReservedDescriptorStateBytes() ==
            reservedBytesBefore);
        const auto segmentCountResult = receiver.BoundSegmentCount(sessionTag);
        REQUIRE(segmentCountResult);
        REQUIRE(segmentCountResult.Value() == 0);
        const auto manifestStateResult = receiver.HasFinalManifest(sessionTag);
        REQUIRE(manifestStateResult);
        REQUIRE_FALSE(manifestStateResult.Value());
    }

    SECTION("record type and payload type mismatch has no fallback")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const auto segmentEnvelopeResult = pbprotocol::ParseControlRecord(
            segmentRecord);
        REQUIRE(segmentEnvelopeResult);
        const std::vector<std::byte> mismatchedRecord = WrapControlPayload(
            pbprotocol::ControlRecordType::SessionDescriptor,
            11,
            sessionTag,
            segmentEnvelopeResult.Value().payload);
        const auto result = receiver.ReceiveControlRecord(mismatchedRecord);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbprotocol::ProtocolErrorCode::TrailingBytes);
        REQUIRE(receiver.ActiveSessionCount() == 0);
        REQUIRE(receiver.ReservedDescriptorStateBytes() == 0);

        RequireInserted(
            receiver.ReceiveControlRecord(sessionRecord),
            pbprotocol::ControlRecordType::SessionDescriptor,
            sessionTag);
        const auto sessionEnvelopeResult = pbprotocol::ParseControlRecord(
            sessionRecord);
        REQUIRE(sessionEnvelopeResult);
        const std::vector<std::byte> segmentWithSessionPayload =
            WrapControlPayload(
                pbprotocol::ControlRecordType::SegmentDescriptor,
                12,
                sessionTag,
                sessionEnvelopeResult.Value().payload);
        const auto segmentMismatchResult = receiver.ReceiveControlRecord(
            segmentWithSessionPayload);
        REQUIRE_FALSE(segmentMismatchResult);
        REQUIRE(segmentMismatchResult.Error().code ==
            pbprotocol::ProtocolErrorCode::TruncatedInput);

        const std::vector<std::byte> manifestWithSegmentPayload =
            WrapControlPayload(
                pbprotocol::ControlRecordType::FinalManifest,
                13,
                sessionTag,
                segmentEnvelopeResult.Value().payload);
        const auto manifestMismatchResult = receiver.ReceiveControlRecord(
            manifestWithSegmentPayload);
        REQUIRE_FALSE(manifestMismatchResult);
        REQUIRE(manifestMismatchResult.Error().code ==
            pbprotocol::ProtocolErrorCode::TrailingBytes);
        const auto segmentCountResult = receiver.BoundSegmentCount(sessionTag);
        REQUIRE(segmentCountResult);
        REQUIRE(segmentCountResult.Value() == 0);
        const auto manifestStateResult = receiver.HasFinalManifest(sessionTag);
        REQUIRE(manifestStateResult);
        REQUIRE_FALSE(manifestStateResult.Value());
    }

    SECTION("unknown Session lookup occurs before Segment payload admission")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        auto malformedSegment = segmentRecord;
        malformedSegment[26] ^= Byte(0x7F);
        const pbprotocol::ControlRecordView malformedView{
            pbprotocol::kControlVersion,
            pbprotocol::ControlRecordType::SegmentDescriptor,
            12,
            sessionTag,
            std::span<const std::byte>(malformedSegment).subspan(
                pbprotocol::kControlRecordPrefixBytes,
                pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes)};
        std::vector<std::byte> validEnvelope(malformedSegment.size());
        REQUIRE(pbprotocol::SerializeControlRecord(
            malformedView,
            validEnvelope));

        const auto result = receiver.ReceiveControlRecord(validEnvelope);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::UnknownSession,
                14});
        REQUIRE(receiver.ActiveSessionCount() == 0);
    }
}

TEST_CASE("ControlPlaneReceiver reassembles out of order and submits each ID once",
          "[pbprotocol][control][receiver][fragment][reassembly]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const std::vector<std::byte> record = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        20);
    const auto fragments = MakeFragments(record, 0x1111222233334444ULL);
    REQUIRE(fragments.size() == 3);

    auto receiver = MakeReceiver(resourcePolicy);
    const std::size_t baselineReassemblyBytes =
        receiver.ControlReassemblyBytesInUse();
    const auto lastFirstResult = receiver.ReceiveControlFragment(
        fragments[2],
        100);
    REQUIRE(lastFirstResult);
    REQUIRE(lastFirstResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::Stored);
    REQUIRE_FALSE(lastFirstResult.Value().admission.has_value());

    const auto exactDuplicate = receiver.ReceiveControlFragment(
        fragments[2],
        100);
    REQUIRE(exactDuplicate);
    REQUIRE(exactDuplicate.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::Repeated);

    const auto firstResult = receiver.ReceiveControlFragment(
        fragments[0],
        101);
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::Stored);

    const auto completedResult = receiver.ReceiveControlFragment(
        fragments[1],
        102);
    REQUIRE(completedResult);
    REQUIRE(completedResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);
    REQUIRE(completedResult.Value().admission.has_value());
    REQUIRE(completedResult.Value().admission->recordType ==
        pbprotocol::ControlRecordType::SessionDescriptor);
    REQUIRE(completedResult.Value().admission->sessionTag == sessionTag);
    REQUIRE(receiver.ActiveSessionCount() == 1);
    REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
    REQUIRE(receiver.ControlReassemblyBytesInUse() > baselineReassemblyBytes);

    const auto submittedDuplicate = receiver.ReceiveControlFragment(
        fragments[0],
        103);
    REQUIRE(submittedDuplicate);
    REQUIRE(submittedDuplicate.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::Repeated);
    REQUIRE_FALSE(submittedDuplicate.Value().admission.has_value());
    REQUIRE(receiver.ActiveSessionCount() == 1);

    const auto repeatedFragments = MakeFragments(
        record,
        0x5555666677778888ULL);
    pbprotocol::ProtocolResult<pbprotocol::ControlFragmentReceiveResult>
        repeatedResult = receiver.ReceiveControlFragment(
            repeatedFragments[0],
            104);
    REQUIRE(repeatedResult);
    repeatedResult = receiver.ReceiveControlFragment(
        repeatedFragments[1],
        105);
    REQUIRE(repeatedResult);
    repeatedResult = receiver.ReceiveControlFragment(
        repeatedFragments[2],
        106);
    REQUIRE(repeatedResult);
    REQUIRE(repeatedResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::DescriptorRepeated);
    REQUIRE(repeatedResult.Value().admission.has_value());
    REQUIRE(repeatedResult.Value().admission->bindDisposition ==
        pbprotocol::DescriptorBindDisposition::Repeated);
}

TEST_CASE("ControlPlaneReceiver latches typed conflicts across RecordIds",
          "[pbprotocol][control][receiver][fragment][descriptor][conflict]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const std::vector<std::byte> sessionRecord = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        70);

    SECTION("SessionDescriptor content conflict is terminal")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        std::uint64_t observationOrdinal = 1;
        const auto firstResult = ReceiveAllFragments(
            receiver,
            sessionRecord,
            0x7001ULL,
            observationOrdinal);
        REQUIRE(firstResult);
        REQUIRE(firstResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);

        pbprotocol::SessionDescriptor changedDescriptor = sessionDescriptor;
        changedDescriptor.originalFileSize++;
        const std::vector<std::byte> changedRecord = MakeSessionControlRecord(
            changedDescriptor,
            resourcePolicy,
            71);
        const auto conflictResult = ReceiveAllFragments(
            receiver,
            changedRecord,
            0x7002ULL,
            observationOrdinal);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
        REQUIRE(receiver.ActiveSessionCount() == 1);

        const auto blockedResult = receiver.ReceiveControlRecord(sessionRecord);
        REQUIRE_FALSE(blockedResult);
        REQUIRE(blockedResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }

    SECTION("SegmentDescriptor content conflict is terminal")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        RequireInserted(
            receiver.ReceiveControlRecord(sessionRecord),
            pbprotocol::ControlRecordType::SessionDescriptor,
            sessionTag);

        const pbprotocol::SegmentDescriptor segmentDescriptor =
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor,
                0,
                0,
                117);
        const std::vector<std::byte> segmentRecord = MakeSegmentControlRecord(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy,
            72);
        std::uint64_t observationOrdinal = 1;
        const auto firstResult = ReceiveAllFragments(
            receiver,
            segmentRecord,
            0x7101ULL,
            observationOrdinal);
        REQUIRE(firstResult);
        REQUIRE(firstResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);

        pbprotocol::SegmentDescriptor changedDescriptor = segmentDescriptor;
        changedDescriptor.rawDigest.bytes[0] ^= Byte(0x01);
        changedDescriptor.encodedDigest.bytes[0] ^= Byte(0x01);
        const std::vector<std::byte> changedRecord = MakeSegmentControlRecord(
            changedDescriptor,
            sessionDescriptor,
            resourcePolicy,
            73);
        const auto conflictResult = ReceiveAllFragments(
            receiver,
            changedRecord,
            0x7102ULL,
            observationOrdinal);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
        const auto segmentCountResult = receiver.BoundSegmentCount(sessionTag);
        REQUIRE(segmentCountResult);
        REQUIRE(segmentCountResult.Value() == 1);

        const pbprotocol::FinalManifest finalManifest =
            pbprotocol::test::MakeFinalManifest(sessionDescriptor);
        const std::vector<std::byte> manifestRecord = MakeManifestControlRecord(
            finalManifest,
            sessionDescriptor,
            resourcePolicy,
            74);
        const auto blockedResult = receiver.ReceiveControlRecord(manifestRecord);
        REQUIRE_FALSE(blockedResult);
        REQUIRE(blockedResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }

    SECTION("FinalManifest content conflict is terminal")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        RequireInserted(
            receiver.ReceiveControlRecord(sessionRecord),
            pbprotocol::ControlRecordType::SessionDescriptor,
            sessionTag);

        const pbprotocol::FinalManifest finalManifest =
            pbprotocol::test::MakeFinalManifest(sessionDescriptor);
        const std::vector<std::byte> manifestRecord = MakeManifestControlRecord(
            finalManifest,
            sessionDescriptor,
            resourcePolicy,
            75);
        std::uint64_t observationOrdinal = 1;
        const auto firstResult = ReceiveAllFragments(
            receiver,
            manifestRecord,
            0x7201ULL,
            observationOrdinal);
        REQUIRE(firstResult);
        REQUIRE(firstResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);

        pbprotocol::FinalManifest changedManifest = finalManifest;
        changedManifest.wholeFileDigest.bytes[0] ^= Byte(0x01);
        const std::vector<std::byte> changedRecord = MakeManifestControlRecord(
            changedManifest,
            sessionDescriptor,
            resourcePolicy,
            76);
        const auto conflictResult = ReceiveAllFragments(
            receiver,
            changedRecord,
            0x7202ULL,
            observationOrdinal);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
        const auto manifestResult = receiver.HasFinalManifest(sessionTag);
        REQUIRE(manifestResult);
        REQUIRE(manifestResult.Value());

        const auto blockedResult = receiver.ReceiveControlRecord(manifestRecord);
        REQUIRE_FALSE(blockedResult);
        REQUIRE(blockedResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }
}

TEST_CASE("ControlPlaneReceiver retains complete unknown-Session records for retry",
          "[pbprotocol][control][receiver][fragment][pending]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            117);
    const std::vector<std::byte> segmentRecord = MakeSegmentControlRecord(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        30);
    const auto segmentFragments = MakeFragments(
        segmentRecord,
        0xA1A2A3A4A5A6A7A8ULL,
        64);

    auto receiver = MakeReceiver(resourcePolicy);
    for (std::size_t fragmentIndex = 0;
         fragmentIndex + 1 < segmentFragments.size();
         fragmentIndex++)
    {
        REQUIRE(receiver.ReceiveControlFragment(
            segmentFragments[fragmentIndex],
            fragmentIndex));
    }
    const auto pendingResult = receiver.ReceiveControlFragment(
        segmentFragments.back(),
        segmentFragments.size());
    REQUIRE_FALSE(pendingResult);
    REQUIRE(pendingResult.Error().code ==
        pbprotocol::ProtocolErrorCode::UnknownSession);
    REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
    REQUIRE(receiver.ActiveSessionCount() == 0);

    const std::vector<std::byte> sessionRecord = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        31);
    RequireInserted(
        receiver.ReceiveControlRecord(sessionRecord),
        pbprotocol::ControlRecordType::SessionDescriptor,
        sessionTag);

    const auto retryResult = receiver.ReceiveControlFragment(
        segmentFragments.front(),
        segmentFragments.size() + 1U);
    REQUIRE(retryResult);
    REQUIRE(retryResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);
    REQUIRE(retryResult.Value().admission.has_value());
    REQUIRE(retryResult.Value().admission->recordType ==
        pbprotocol::ControlRecordType::SegmentDescriptor);
    const auto segmentCountResult = receiver.BoundSegmentCount(sessionTag);
    REQUIRE(segmentCountResult);
    REQUIRE(segmentCountResult.Value() == 1);
}

TEST_CASE("ControlPlaneReceiver retries only transient admission capacity failures",
          "[pbprotocol][control][receiver][fragment][pending][resource]")
{
    const pbprotocol::ReceiverResourcePolicy serializationPolicy =
        pbprotocol::test::MakeResourcePolicy();

    SECTION("receiver-wide Session capacity can be released and retried")
    {
        auto receiverPolicy = serializationPolicy;
        receiverPolicy.maxConcurrentSessions = 1;
        const pbprotocol::SessionDescriptor firstSession =
            pbprotocol::test::MakeSessionDescriptor(117, 1);
        pbprotocol::SessionDescriptor secondSession = firstSession;
        secondSession.sessionId.bytes.back() = Byte(0xEF);
        const std::vector<std::byte> firstRecord = MakeSessionControlRecord(
            firstSession,
            receiverPolicy,
            32);
        const std::vector<std::byte> secondRecord = MakeSessionControlRecord(
            secondSession,
            receiverPolicy,
            33);

        auto receiver = MakeReceiver(receiverPolicy);
        REQUIRE(receiver.ReceiveControlRecord(firstRecord));
        std::uint64_t observationOrdinal = 1;
        const auto capacityResult = ReceiveAllFragments(
            receiver,
            secondRecord,
            0xCAFEBABEULL,
            observationOrdinal);
        REQUIRE_FALSE(capacityResult);
        REQUIRE(capacityResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.ActiveSessionCount() == 1);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);

        const auto removalResult = receiver.RemoveSession(firstSession.sessionId);
        REQUIRE(removalResult);
        REQUIRE(removalResult.Value());
        const std::vector<std::byte> duplicateFragment = MakeFragmentBytes(
            secondRecord,
            0xCAFEBABEULL,
            0,
            64);
        const auto retryResult = receiver.ReceiveControlFragment(
            duplicateFragment,
            observationOrdinal);
        REQUIRE(retryResult);
        REQUIRE(retryResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::DescriptorInserted);
        REQUIRE(receiver.ActiveSessionCount() == 1);
    }

    SECTION("descriptor policy violation is terminal rather than retryable")
    {
        auto strictPolicy = serializationPolicy;
        strictPolicy.maxAcceptedFileBytes = 100;
        // Cross-field invariant: prompt threshold must stay <= file cap.
        strictPolicy.maxOutputPreallocationBytesWithoutPrompt = 50;
        const pbprotocol::SessionDescriptor oversizedSession =
            pbprotocol::test::MakeSessionDescriptor(117, 1);
        const std::vector<std::byte> record = MakeSessionControlRecord(
            oversizedSession,
            serializationPolicy,
            34);
        const auto fragments = MakeFragments(
            record,
            0xDEADBEEFULL,
            64);

        auto receiver = MakeReceiver(strictPolicy);
        for (std::size_t fragmentIndex = 0;
             fragmentIndex + 1 < fragments.size();
             fragmentIndex++)
        {
            REQUIRE(receiver.ReceiveControlFragment(
                fragments[fragmentIndex],
                fragmentIndex));
        }
        const auto policyResult = receiver.ReceiveControlFragment(
            fragments.back(),
            fragments.size());
        REQUIRE_FALSE(policyResult);
        REQUIRE(policyResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.ActiveSessionCount() == 0);

        const auto tombstoneResult = receiver.ReceiveControlFragment(
            fragments.front(),
            fragments.size() + 1U);
        REQUIRE_FALSE(tombstoneResult);
        REQUIRE(tombstoneResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
    }
}

TEST_CASE("ControlPlaneReceiver quarantines fragment and semantic conflicts",
          "[pbprotocol][control][receiver][fragment][conflict]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const std::vector<std::byte> record = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        40);

    SECTION("same index with different CRC-valid payload conflicts")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();
        const auto fragments = MakeFragments(record, 0xABCDEFULL);
        REQUIRE(receiver.ReceiveControlFragment(fragments[0], 1));
        const std::size_t bytesBeforeConflict =
            receiver.ControlReassemblyBytesInUse();

        const auto parsedResult = pbprotocol::ParseControlFragment(fragments[0]);
        REQUIRE(parsedResult);
        std::vector<std::byte> changedPayload(
            parsedResult.Value().payload.begin(),
            parsedResult.Value().payload.end());
        changedPayload[0] ^= Byte(0x01);
        pbprotocol::ControlFragmentView changedFragment = parsedResult.Value();
        changedFragment.payload = changedPayload;
        std::vector<std::byte> changedBytes = MakeFragmentBytes(
            record,
            0xABCDEFULL,
            0);
        REQUIRE(pbprotocol::SerializeControlFragment(
            changedFragment,
            changedBytes));

        const auto conflictResult = receiver.ReceiveControlFragment(
            changedBytes,
            2);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
        REQUIRE(receiver.ControlReassemblyBytesInUse() < bytesBeforeConflict);

        const auto futureResult = receiver.ReceiveControlFragment(
            fragments[0],
            3);
        REQUIRE_FALSE(futureResult);
        REQUIRE(futureResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
        receiver.ResetControlReassembly();
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(receiver.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);
    }

    SECTION("same RecordId with different CRC-valid metadata conflicts")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();
        const auto fragments = MakeFragments(record, 0xABCD01ULL);
        REQUIRE(receiver.ReceiveControlFragment(fragments[0], 1));
        const auto parsedResult = pbprotocol::ParseControlFragment(fragments[0]);
        REQUIRE(parsedResult);
        pbprotocol::ControlFragmentView changedMetadata = parsedResult.Value();
        changedMetadata.totalRecordBytes++;
        std::vector<std::byte> changedBytes(fragments[0].size());
        REQUIRE(pbprotocol::SerializeControlFragment(
            changedMetadata,
            changedBytes));

        const auto result = receiver.ReceiveControlFragment(changedBytes, 2);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ControlFragmentConflict,
                12});
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
        REQUIRE(receiver.ControlReassemblyBytesInUse() >
            baselineReassemblyBytes);
    }

    SECTION("cumulative bytes must equal the declared total exactly")
    {
        const auto serializeFragment = [](
            const pbprotocol::ControlFragmentView& fragment)
        {
            const auto sizeResult = pbprotocol::GetSerializedSize(fragment);
            REQUIRE(sizeResult);
            std::vector<std::byte> bytes(sizeResult.Value());
            REQUIRE(pbprotocol::SerializeControlFragment(fragment, bytes));
            return bytes;
        };

        const std::array<std::byte, 20> longPayload{Byte(0xA5)};
        const std::vector<std::byte> longFirst = serializeFragment(
            pbprotocol::ControlFragmentView{7, 0, 2, 30, 0, longPayload});
        const std::vector<std::byte> longSecond = serializeFragment(
            pbprotocol::ControlFragmentView{7, 1, 2, 30, 0, longPayload});
        auto excessReceiver = MakeReceiver(resourcePolicy);
        REQUIRE(excessReceiver.ReceiveControlFragment(longFirst, 1));
        const auto excessResult = excessReceiver.ReceiveControlFragment(
            longSecond,
            2);
        REQUIRE_FALSE(excessResult);
        REQUIRE(excessResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);

        const std::array<std::byte, 10> shortPayload{Byte(0x5A)};
        const std::vector<std::byte> shortFirst = serializeFragment(
            pbprotocol::ControlFragmentView{8, 0, 2, 30, 0, shortPayload});
        const std::vector<std::byte> shortSecond = serializeFragment(
            pbprotocol::ControlFragmentView{8, 1, 2, 30, 0, shortPayload});
        auto deficitReceiver = MakeReceiver(resourcePolicy);
        REQUIRE(deficitReceiver.ReceiveControlFragment(shortFirst, 1));
        const auto deficitResult = deficitReceiver.ReceiveControlFragment(
            shortSecond,
            2);
        REQUIRE_FALSE(deficitResult);
        REQUIRE(deficitResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);

        const std::array<std::byte, 20> feasibleFirstPayload{Byte(0x31)};
        const std::array<std::byte, 10> feasibleSecondPayload{Byte(0x32)};
        const std::vector<std::byte> feasibleFirst = serializeFragment(
            pbprotocol::ControlFragmentView{
                9,
                0,
                3,
                30,
                0,
                feasibleFirstPayload});
        const std::vector<std::byte> impossibleSecond = serializeFragment(
            pbprotocol::ControlFragmentView{
                9,
                1,
                3,
                30,
                0,
                feasibleSecondPayload});
        auto earlyConflictReceiver = MakeReceiver(resourcePolicy);
        REQUIRE(earlyConflictReceiver.ReceiveControlFragment(
            feasibleFirst,
            1));
        const auto earlyConflictResult =
            earlyConflictReceiver.ReceiveControlFragment(
                impossibleSecond,
                2);
        REQUIRE_FALSE(earlyConflictResult);
        REQUIRE(earlyConflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
    }

    SECTION("complete wrong-tag record becomes a terminal tombstone")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const pbprotocol::SessionTag wrongTag{sessionTag.value ^ 1ULL};
        const std::vector<std::byte> wrongRecord = MakeSessionControlRecord(
            sessionDescriptor,
            resourcePolicy,
            41,
            &wrongTag);
        const auto fragments = MakeFragments(wrongRecord, 0x123456ULL);
        for (std::size_t fragmentIndex = 0;
             fragmentIndex + 1 < fragments.size();
             fragmentIndex++)
        {
            REQUIRE(receiver.ReceiveControlFragment(
                fragments[fragmentIndex],
                fragmentIndex));
        }
        const auto tagResult = receiver.ReceiveControlFragment(
            fragments.back(),
            fragments.size());
        REQUIRE_FALSE(tagResult);
        REQUIRE(tagResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagMismatch);
        REQUIRE(receiver.ActiveSessionCount() == 0);
        REQUIRE(receiver.ReservedDescriptorStateBytes() == 0);

        const auto tombstoneResult = receiver.ReceiveControlFragment(
            fragments.front(),
            fragments.size() + 1U);
        REQUIRE_FALSE(tombstoneResult);
        REQUIRE(tombstoneResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
    }
}

TEST_CASE("ControlPlaneReceiver enforces observation and resource quotas",
          "[pbprotocol][control][receiver][fragment][resource]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxConcurrentControlReassemblies = 1;
    resourcePolicy.maxControlReassemblyInactivityObservations = 2;
    const std::vector<std::byte> record = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        50);
    const std::vector<std::byte> firstIdFragment = MakeFragmentBytes(
        record,
        1,
        0);
    const std::vector<std::byte> secondIdFragment = MakeFragmentBytes(
        record,
        2,
        0);

    SECTION("concurrency, exact expiry boundary, ordinal regression, and reset")
    {
        auto receiver = MakeReceiver(resourcePolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();
        REQUIRE(receiver.ReceiveControlFragment(firstIdFragment, 10));
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);

        const auto quotaResult = receiver.ReceiveControlFragment(
            secondIdFragment,
            10);
        REQUIRE_FALSE(quotaResult);
        REQUIRE(quotaResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);

        REQUIRE(receiver.AdvanceObservationOrdinal(12));
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
        REQUIRE(receiver.AdvanceObservationOrdinal(13));
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(receiver.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);

        const auto regressionStatus = receiver.AdvanceObservationOrdinal(12);
        REQUIRE_FALSE(regressionStatus);
        REQUIRE(regressionStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidObservationOrdinal);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);

        receiver.ResetControlReassembly();
        REQUIRE(receiver.ReceiveControlFragment(firstIdFragment, 1));
    }

    SECTION("local fragment count is checked before state allocation")
    {
        resourcePolicy.maxControlFragmentsPerRecord = 2;
        auto receiver = MakeReceiver(resourcePolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();
        const auto result = receiver.ReceiveControlFragment(
            firstIdFragment,
            1);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(receiver.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);
    }

    SECTION("sparse maximum wire index does not allocate a dense slot vector")
    {
        constexpr std::uint16_t maximumFragmentCount =
            std::numeric_limits<std::uint16_t>::max();
        auto maximumCountPolicy = resourcePolicy;
        maximumCountPolicy.maxControlRecordBytes =
            pbprotocol::kMaximumControlRecordBytes;
        maximumCountPolicy.maxControlFragmentsPerRecord =
            maximumFragmentCount;
        auto receiver = MakeReceiver(maximumCountPolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();

        const std::array<std::byte, 1> payload{Byte(0x5A)};
        const pbprotocol::ControlFragmentView fragment{
            3,
            static_cast<std::uint16_t>(maximumFragmentCount - 1U),
            maximumFragmentCount,
            static_cast<std::uint32_t>(
                pbprotocol::kMaximumControlRecordBytes),
            0,
            payload};
        std::array<std::byte, pbprotocol::kMinimumControlFragmentBytes>
            fragmentBytes{};
        REQUIRE(pbprotocol::SerializeControlFragment(fragment, fragmentBytes));

        const auto result = receiver.ReceiveControlFragment(fragmentBytes, 1);
        REQUIRE(result);
        REQUIRE(result.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::Stored);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 1);
        REQUIRE(receiver.ControlReassemblyBytesInUse() >
            baselineReassemblyBytes);
        REQUIRE(receiver.ControlReassemblyBytesInUse() <=
            maximumCountPolicy.maxControlReassemblyBytes);

        receiver.ResetControlReassembly();
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(receiver.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);
    }

    SECTION("local complete-record limit applies to both ingress paths")
    {
        auto localLimitPolicy = resourcePolicy;
        localLimitPolicy.maxControlRecordBytes =
            static_cast<std::uint32_t>(record.size() - 1U);
        localLimitPolicy.maxControlFragmentsPerRecord =
            localLimitPolicy.maxControlRecordBytes;
        auto receiver = MakeReceiver(localLimitPolicy);
        const std::size_t baselineReassemblyBytes =
            receiver.ControlReassemblyBytesInUse();

        const auto completeResult = receiver.ReceiveControlRecord(record);
        REQUIRE_FALSE(completeResult);
        REQUIRE(completeResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        const auto fragmentResult = receiver.ReceiveControlFragment(
            firstIdFragment,
            1);
        REQUIRE_FALSE(fragmentResult);
        REQUIRE(fragmentResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.ActiveSessionCount() == 0);
        REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(receiver.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);
    }

    SECTION("bounded PMR distinguishes quota from upstream exhaustion")
    {
        auto tinyPolicy = resourcePolicy;
        tinyPolicy.maxControlRecordBytes =
            static_cast<std::uint32_t>(record.size());
        tinyPolicy.maxControlReassemblyBytes = record.size();
        tinyPolicy.maxControlFragmentsPerRecord = record.size();
        auto tinyReceiver = MakeReceiver(tinyPolicy);
        const std::size_t tinyBaselineBytes =
            tinyReceiver.ControlReassemblyBytesInUse();
        const auto quotaResult = tinyReceiver.ReceiveControlFragment(
            firstIdFragment,
            1);
        REQUIRE_FALSE(quotaResult);
        REQUIRE(quotaResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded);
        REQUIRE(tinyReceiver.ActiveControlReassemblyCount() == 0);
        REQUIRE(tinyReceiver.ControlReassemblyBytesInUse() ==
            tinyBaselineBytes);

        auto failingResource = std::make_shared<TrackingMemoryResource>();
        auto failingReceiverResult =
            pbprotocol::ControlPlaneReceiver::CreateWithMemoryResource(
                resourcePolicy,
                failingResource);
        REQUIRE(failingReceiverResult);
        auto failingReceiver = std::move(failingReceiverResult).Value();
        const std::size_t failingBaselineBytes =
            failingReceiver.ControlReassemblyBytesInUse();

        const auto firstStoredResult = failingReceiver.ReceiveControlFragment(
            firstIdFragment,
            1);
        REQUIRE(firstStoredResult);
        REQUIRE(firstStoredResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::Stored);
        const std::size_t bytesBeforeFailure =
            failingReceiver.ControlReassemblyBytesInUse();

        const std::vector<std::byte> nextFragment = MakeFragmentBytes(
            record,
            1,
            1);
        failingResource->SetFailAllocation(true);
        const auto exhaustionResult = failingReceiver.ReceiveControlFragment(
            nextFragment,
            2);
        REQUIRE_FALSE(exhaustionResult);
        REQUIRE(exhaustionResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceExhausted);
        REQUIRE(failingReceiver.ActiveControlReassemblyCount() == 1);
        REQUIRE(failingReceiver.ControlReassemblyBytesInUse() ==
            bytesBeforeFailure);

        failingResource->SetFailAllocation(false);
        const auto recoveredResult = failingReceiver.ReceiveControlFragment(
            nextFragment,
            3);
        REQUIRE(recoveredResult);
        REQUIRE(recoveredResult.Value().disposition ==
            pbprotocol::ControlFragmentReceiveDisposition::Stored);

        const auto parsedFirstResult = pbprotocol::ParseControlFragment(
            firstIdFragment);
        REQUIRE(parsedFirstResult);
        std::vector<std::byte> conflictingPayload(
            parsedFirstResult.Value().payload.begin(),
            parsedFirstResult.Value().payload.end());
        conflictingPayload[0] ^= Byte(0x01);
        pbprotocol::ControlFragmentView conflictingFragment =
            parsedFirstResult.Value();
        conflictingFragment.payload = conflictingPayload;
        std::vector<std::byte> conflictingBytes(firstIdFragment.size());
        REQUIRE(pbprotocol::SerializeControlFragment(
            conflictingFragment,
            conflictingBytes));

        failingResource->SetFailAllocation(true);
        const auto conflictResult = failingReceiver.ReceiveControlFragment(
            conflictingBytes,
            4);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ControlFragmentConflict);
        REQUIRE(failingReceiver.ActiveControlReassemblyCount() == 1);

        // Conflict cleanup and reset must only deallocate. A PMR upstream that
        // rejects every new allocation must not turn either path into terminate.
        failingReceiver.ResetControlReassembly();
        failingResource->SetFailAllocation(false);
        REQUIRE(failingReceiver.ControlReassemblyBytesInUse() ==
            failingBaselineBytes);

        auto expiryPolicy = resourcePolicy;
        expiryPolicy.maxConcurrentControlReassemblies = 2;
        expiryPolicy.maxControlReassemblyInactivityObservations = 1;
        auto expiryResource = std::make_shared<TrackingMemoryResource>();
        auto expiryReceiverResult =
            pbprotocol::ControlPlaneReceiver::CreateWithMemoryResource(
                expiryPolicy,
                expiryResource);
        REQUIRE(expiryReceiverResult);
        auto expiryReceiver = std::move(expiryReceiverResult).Value();
        const std::size_t expiryBaselineBytes =
            expiryReceiver.ControlReassemblyBytesInUse();
        REQUIRE(expiryReceiver.ReceiveControlFragment(firstIdFragment, 1));
        REQUIRE(expiryReceiver.ReceiveControlFragment(secondIdFragment, 2));
        REQUIRE(expiryReceiver.ActiveControlReassemblyCount() == 2);

        // Erasing the first entry shifts a live sparse-map record. Expiry and
        // reset must still be deallocation-only when upstream rejects every
        // new allocation.
        expiryResource->SetFailAllocation(true);
        REQUIRE(expiryReceiver.AdvanceObservationOrdinal(3));
        REQUIRE(expiryReceiver.ActiveControlReassemblyCount() == 1);
        expiryReceiver.ResetControlReassembly();
        REQUIRE(expiryReceiver.ActiveControlReassemblyCount() == 0);
        expiryResource->SetFailAllocation(false);
        REQUIRE(expiryReceiver.ControlReassemblyBytesInUse() ==
            expiryBaselineBytes);
    }
}

TEST_CASE("ControlPlaneReceiver move and reset preserve PMR lifetime ordering",
          "[pbprotocol][control][receiver][fragment][resource][lifetime]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const std::vector<std::byte> record = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        60);
    const std::vector<std::byte> fragment = MakeFragmentBytes(record, 7, 0);
    auto trackingResource = std::make_shared<TrackingMemoryResource>();

    {
        auto receiverResult =
            pbprotocol::ControlPlaneReceiver::CreateWithMemoryResource(
                resourcePolicy,
                trackingResource);
        REQUIRE(receiverResult);
        auto source = std::move(receiverResult).Value();
        const std::size_t baselineReassemblyBytes =
            source.ControlReassemblyBytesInUse();
        REQUIRE(trackingResource->BytesInUse() == baselineReassemblyBytes);
        REQUIRE(source.ReceiveControlFragment(fragment, 1));
        REQUIRE(trackingResource->BytesInUse() > baselineReassemblyBytes);

        auto destination = std::move(source);
        const auto movedFromResult = source.ReceiveControlFragment(fragment, 2);
        REQUIRE_FALSE(movedFromResult);
        REQUIRE(movedFromResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation);
        REQUIRE(destination.ActiveControlReassemblyCount() == 1);
        destination.ResetControlReassembly();
        REQUIRE(destination.ControlReassemblyBytesInUse() ==
            baselineReassemblyBytes);
        REQUIRE(trackingResource->BytesInUse() == baselineReassemblyBytes);
    }
    REQUIRE(trackingResource->BytesInUse() == 0);
}

TEST_CASE("ControlPlaneReceiver counts resource-policy rejections on both paths",
          "[pbprotocol][control][receiver][telemetry]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::ReceiverResourcePolicy basePolicy =
        pbprotocol::test::MakeResourcePolicy();

    SECTION("complete record above maxControlRecordBytes counts once")
    {
        const std::vector<std::byte> record = MakeSessionControlRecord(
            sessionDescriptor,
            basePolicy);
        pbprotocol::ReceiverResourcePolicy tinyPolicy = basePolicy;
        tinyPolicy.maxControlRecordBytes =
            static_cast<std::uint32_t>(record.size() - 1);
        // Cross-field invariant: fragment count must stay <= record bytes.
        tinyPolicy.maxControlFragmentsPerRecord =
            tinyPolicy.maxControlRecordBytes;
        auto receiver = MakeReceiver(tinyPolicy);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 0);

        const auto oversizedResult = receiver.ReceiveControlRecord(record);
        REQUIRE_FALSE(oversizedResult);
        REQUIRE(oversizedResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 1);

        // Protocol-level parse failures are not resource-policy rejections.
        const auto truncatedResult = receiver.ReceiveControlRecord(
            std::span<const std::byte>(record).first(record.size() - 1));
        REQUIRE_FALSE(truncatedResult);
        REQUIRE(truncatedResult.Error().code !=
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 1);
    }

    SECTION("fragment quota rejection counts exactly once per operation")
    {
        const std::vector<std::byte> record = MakeSessionControlRecord(
            sessionDescriptor,
            basePolicy);
        pbprotocol::ReceiverResourcePolicy tinyPolicy = basePolicy;
        tinyPolicy.maxControlRecordBytes =
            static_cast<std::uint32_t>(record.size() - 1);
        // Cross-field invariant: fragment count must stay <= record bytes.
        tinyPolicy.maxControlFragmentsPerRecord =
            tinyPolicy.maxControlRecordBytes;
        auto receiver = MakeReceiver(tinyPolicy);

        const std::vector<std::byte> fragment = MakeFragmentBytes(
            record,
            42,
            0);
        const auto quotaResult = receiver.ReceiveControlFragment(fragment, 1);
        REQUIRE_FALSE(quotaResult);
        REQUIRE(quotaResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 1);

        // A second rejected fragment is a separate receive operation.
        const auto repeatedQuotaResult = receiver.ReceiveControlFragment(
            fragment,
            2);
        REQUIRE_FALSE(repeatedQuotaResult);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 2);
    }

    SECTION("successful record and fragment admissions keep the counter at zero")
    {
        auto receiver = MakeReceiver(basePolicy);
        const std::vector<std::byte> record = MakeSessionControlRecord(
            sessionDescriptor,
            basePolicy);
        REQUIRE(receiver.ReceiveControlRecord(record));
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 0);

        // An identical second session record through the fragment path is a
        // repeated admission, not a conflict or a quota rejection.
        const std::vector<std::byte> secondRecord = MakeSessionControlRecord(
            sessionDescriptor,
            basePolicy,
            2);
        std::uint64_t observationOrdinal = 1;
        const auto finalResult = ReceiveAllFragments(
            receiver,
            secondRecord,
            99,
            observationOrdinal);
        REQUIRE(finalResult);
        REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 0);
    }
}
