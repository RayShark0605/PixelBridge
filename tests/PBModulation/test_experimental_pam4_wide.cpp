#include "pbmodulation/experimental_pam4_wide.h"
#include "pbmodulation/experimental_pam4.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/transport_block_codec.h"
#include "unified_gray_resampling_fixture.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace
{
using namespace pbmodulation;

struct Fixture
{
    pbprotocol::SessionDescriptor session;
    std::array<std::byte, 44> bootstrap{};
    std::array<std::byte, kExperimentalPam4WideCodedFrameBytes> coded{};
    std::array<std::vector<std::byte>, kExperimentalPam4WideCodewordCount> blocks;
    std::vector<std::byte> raster;
};

std::vector<std::byte> Wrap(const pbprotocol::SessionTag tag, const pbprotocol::ControlRecordType type,
    const std::span<const std::byte> payload, const std::uint64_t sequence = 7)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion, type, sequence, tag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

std::array<UnifiedFrameSlotInput, kExperimentalPam4WideCodewordCount> Slots(const Fixture& fixture)
{
    std::array<UnifiedFrameSlotInput, kExperimentalPam4WideCodewordCount> slots{};
    for (std::uint32_t slot = 0; slot < slots.size(); slot++)
    {
        slots[slot] = {{slot, slot == 0 ? UnifiedSlotKind::Control : UnifiedSlotKind::Transport,
            slot == 0 ? UnifiedControlPriority::SessionDescriptor : UnifiedControlPriority::NotApplicable}, true, fixture.blocks[slot]};
    }
    return slots;
}

Fixture MakeFixture(const std::uint64_t sequence = 3267, const std::uint64_t fileBytes = 1000000)
{
    Fixture fixture;
    fixture.session.protocolVersion = pbprotocol::GetProtocolVersion();
    for (std::size_t index = 0; index < fixture.session.sessionId.bytes.size(); index++)
    {
        fixture.session.sessionId.bytes[index] = static_cast<std::byte>(index * 13 + 17);
    }
    fixture.session.originalFileSize = fileBytes;
    fixture.session.segmentCount = 1;
    fixture.session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
    fixture.session.sessionVisualProfileId = pbprotocol::kPam4WideExperimentalProfile.visualProfileId;
    const auto tag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    const auto sessionSize = pbprotocol::GetSerializedSize(fixture.session);
    REQUIRE(sessionSize);
    std::vector<std::byte> payload(sessionSize.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(fixture.session, payload));
    fixture.blocks[0] = Wrap(tag, pbprotocol::ControlRecordType::SessionDescriptor, payload);
    for (std::uint32_t slot = 1; slot < fixture.blocks.size(); slot++)
    {
        payload.resize(kExperimentalPam4WideTransportPayloadBytes - slot % 5);
        for (std::size_t index = 0; index < payload.size(); index++)
        {
            payload[index] = static_cast<std::byte>((index * 29 + slot * 71 + sequence * 13) & 255);
        }
        const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData, pbprotocol::kTransportProtocolMinor,
            0, tag, 0, slot + static_cast<std::uint32_t>(sequence % 1000), static_cast<std::uint16_t>(payload.size())};
        fixture.blocks[slot].resize(pbprotocol::GetTransportSerializedSize(header));
        REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, fixture.blocks[slot]));
    }
    pbprotocol::BootstrapRecord bootstrap;
    bootstrap.protocolVersion = pbprotocol::GetProtocolVersion();
    bootstrap.visualProfileId = fixture.session.sessionVisualProfileId;
    bootstrap.visualLayoutVersion = pbprotocol::kPam4WideExperimentalProfile.visualLayoutVersion;
    bootstrap.sessionTag = tag;
    bootstrap.frameSequence = sequence;
    REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, fixture.bootstrap));
    const auto slots = Slots(fixture);
    REQUIRE(PackExperimentalPam4WideFrame({fixture.bootstrap, slots}, fixture.coded));
    fixture.raster.resize(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeExperimentalPam4WideFrame(fixture.bootstrap, fixture.coded, fixture.raster));
    return fixture;
}

ExperimentalPam4WideCpuDecoder MakeDecoder()
{
    auto result = ExperimentalPam4WideCpuDecoder::Create(ExperimentalPam4WideCpuDecoder::RequiredBytes());
    REQUIRE(result);
    return std::move(result.Value());
}

LumaView View(const std::vector<std::byte>& raster)
{
    return {raster, 1920, 1080, 1920 * 4, LumaPixelFormat::Bgra8};
}

std::vector<std::byte> FixedWidePresentationCapture(const std::span<const std::byte> canonical, const std::uint32_t offsetX)
{
    REQUIRE(canonical.size() == kUnifiedFrameBgraBytes);
    REQUIRE(offsetX <= 1);
    constexpr double remoteScale = 0.851;
    constexpr double originX = 47.37;
    constexpr double originY = 31.61;
    const auto Read = [canonical, offsetX](const int column, const int row) -> double
    {
        if (column < 0 || column >= 2560 || row < 0 || row >= 1600)
        {
            return 0;
        }
        const int shiftedColumn = column - static_cast<int>(offsetX);
        unsigned value = 128;
        if (shiftedColumn >= 0 && row >= 80 && row < 1520)
        {
            const auto sourceX = static_cast<std::uint32_t>((static_cast<std::uint64_t>(shiftedColumn) * 2 + 1) * 1920 / (2560 * 2));
            const auto sourceY = static_cast<std::uint32_t>((static_cast<std::uint64_t>(row - 80) * 2 + 1) * 1080 / (1440 * 2));
            value = std::to_integer<unsigned>(canonical[(static_cast<std::size_t>(sourceY) * 1920 + sourceX) * 4]);
        }
        const double limited = std::floor(16.0 + value * (219.0 / 255.0) + 0.5);
        return std::floor(std::clamp((limited - 16) * (255.0 / 219.0), 0.0, 255.0) + 0.5);
    };
    std::vector<std::byte> captured(2560ULL * 1440 * 4);
    for (std::uint32_t row = 0; row < 1440; row++)
    {
        const double logicalY = (row + 0.5 - originY) / remoteScale - 0.5;
        const int top = static_cast<int>(std::floor(logicalY));
        const double fractionY = logicalY - top;
        for (std::uint32_t column = 0; column < 2560; column++)
        {
            const double logicalX = (column + 0.5 - originX) / remoteScale - 0.5;
            const int left = static_cast<int>(std::floor(logicalX));
            const double fractionX = logicalX - left;
            const double upper = Read(left, top) * (1 - fractionX) + Read(left + 1, top) * fractionX;
            const double lower = Read(left, top + 1) * (1 - fractionX) + Read(left + 1, top + 1) * fractionX;
            const auto value = static_cast<std::byte>(static_cast<unsigned>(std::floor(upper * (1 - fractionY) + lower * fractionY + 0.5)));
            const std::size_t offset = (static_cast<std::size_t>(row) * 2560 + column) * 4;
            captured[offset] = value;
            captured[offset + 1] = value;
            captured[offset + 2] = value;
            captured[offset + 3] = std::byte{255};
        }
    }
    return captured;
}

void Exact(const Fixture& fixture, const ExperimentalPam4WideObservation& observation, const ExperimentalPam4WideCpuDecoder& decoder)
{
    INFO("bootstrap=" << static_cast<unsigned>(observation.bootstrap.erasure));
    INFO("frame=" << static_cast<unsigned>(observation.frameErasure));
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.acceptedBlocks == 20);
    REQUIRE(observation.acceptedControlRecords == 1);
    REQUIRE(observation.acceptedTransportBlocks == 19);
    REQUIRE(observation.frameSlotCount == 20);
    const auto blocks = decoder.GetAcceptedBlocks();
    REQUIRE(blocks.size() == 20);
    for (const auto& block : blocks)
    {
        REQUIRE(block.codewordSlot < fixture.blocks.size());
        REQUIRE(block.size == fixture.blocks[block.codewordSlot].size());
        REQUIRE(std::equal(fixture.blocks[block.codewordSlot].begin(), fixture.blocks[block.codewordSlot].end(), block.bytes.begin()));
    }
    REQUIRE(decoder.GetSoftMetrics().size() == kExperimentalPam4WideMetricCount);
    REQUIRE(decoder.GetCalibration().valid);
}

std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto value : bytes)
    {
        result.push_back(digits[std::to_integer<unsigned>(value) >> 4]);
        result.push_back(digits[std::to_integer<unsigned>(value) & 15]);
    }
    return result;
}

void CopyRegion(const std::vector<std::byte>& source, std::vector<std::byte>& destination, const UnifiedPixelRegion region)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        const auto offset = (static_cast<std::size_t>(region.y + row) * 1920 + region.x) * 4;
        std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), region.width * 4, destination.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}
}

TEST_CASE("PAM4 Wide identity, capacity and frozen mapping remain outside old catalogs", "[pam4-wide][contract]")
{
    CHECK(pbprotocol::kPam4WideExperimentalProfile.visualLayoutVersion == 16);
    CHECK_FALSE(pbprotocol::IsProductSessionVisualProfileId(pbprotocol::kPam4WideExperimentalProfile.visualProfileId));
    CHECK_FALSE(IsUnifiedGrayCarrierPair(pbprotocol::kPam4WideExperimentalProfile.visualProfileId, 16));
    CHECK(kExperimentalPam4WideTransportPayloadPerFrame == 30951);
    std::vector<bool> sites(640 * 360);
    std::vector<bool> positions(kExperimentalPam4WideCellCount * 2);
    std::array<std::byte, 4> serialized{};
    pbprotocol::Blake3Hasher siteHasher;
    pbprotocol::Blake3Hasher positionHasher;
    for (std::uint32_t cell = 0; cell < kExperimentalPam4WideCellCount; cell++)
    {
        const auto site = GetExperimentalPam4WideCellSite(cell);
        REQUIRE(site < sites.size());
        REQUIRE_FALSE(sites[site]);
        sites[site] = true;
        const auto x = site % 640 * 3;
        const auto y = site / 640 * 3;
        REQUIRE(std::ranges::any_of(kUnifiedVisualProfile.regions, [x, y](const auto& region)
        {
            return region.kind == UnifiedRegionKind::Data && x >= region.bounds.x && y >= region.bounds.y &&
                x + 3 <= region.bounds.x + region.bounds.width && y + 3 <= region.bounds.y + region.bounds.height;
        }));
        for (std::uint32_t byte = 0; byte < 4; byte++)
        {
            serialized[byte] = static_cast<std::byte>((site >> (byte * 8)) & 255);
        }
        siteHasher.Update(serialized);
    }
    for (std::uint32_t bit = 0; bit < kExperimentalPam4WideMetricCount; bit++)
    {
        const auto position = GetExperimentalPam4WideBitPosition(bit);
        REQUIRE(position < positions.size());
        REQUIRE_FALSE(positions[position]);
        positions[position] = true;
        for (std::uint32_t byte = 0; byte < 4; byte++)
        {
            serialized[byte] = static_cast<std::byte>((position >> (byte * 8)) & 255);
        }
        positionHasher.Update(serialized);
    }
    CHECK(GetExperimentalPam4WideCellSite(kExperimentalPam4WideCellCount) == UINT32_MAX);
    CHECK(GetExperimentalPam4WideBitPosition(UINT32_MAX) == UINT32_MAX);
    CHECK(Hex(siteHasher.Finalize()) == "59004dfa5c61f2d2e3fec74649557717a34f22d9c59c07c87a99ae4d44d698d0");
    CHECK(Hex(positionHasher.Finalize()) == "2a89e25e47fc5848c5c606962debfc51d80ace9ed9c5b464b0a28daf6a6f30fd");
}

TEST_CASE("PAM4 Wide typed Session crosses pixels, CRC and authoritative Control admission", "[pam4-wide][typed]")
{
    const auto fixture = MakeFixture();
    auto decoder = MakeDecoder();
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    auto receiver = pbprotocol::ControlPlaneReceiver::Create(pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(receiver);
    const auto control = decoder.GetAcceptedBlocks().front();
    const auto admission = receiver.Value().ReceiveControlRecord(std::span(control.bytes).first(control.size));
    REQUIRE(admission);
    REQUIRE(receiver.Value().ActiveSessionCount() == 1);
    const auto receivedSession = receiver.Value().GetSessionDescriptor(pbprotocol::DeriveSessionTag(fixture.session.sessionId));
    REQUIRE(receivedSession);
    CHECK(receivedSession.Value() == fixture.session);
    CHECK(receiver.Value().ReceiveControlRecord(std::span(control.bytes).first(control.size)));
    CHECK(Hex(pbprotocol::ComputeBlake3Digest(fixture.raster)) == "221562b0fc98ebd1b32eaf5edd10a62e804fb3aeaa690be47590392268204937");
    CHECK(Hex(pbprotocol::ComputeBlake3Digest(fixture.coded)) == "709468210ebebca35477678e6599c03051660cf1060aa498cb12c4f60ba626df");
    auto sessionConflict = fixture.session;
    sessionConflict.fileNameUtf8 = "other.bin";
    const auto size = pbprotocol::GetSerializedSize(sessionConflict);
    REQUIRE(size);
    std::vector<std::byte> payload(size.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(sessionConflict, payload));
    const auto conflict = Wrap(admission.Value().sessionTag, pbprotocol::ControlRecordType::SessionDescriptor, payload);
    CHECK_FALSE(receiver.Value().ReceiveControlRecord(conflict));
}

TEST_CASE("PAM4 Wide Control envelope cannot substitute for typed Descriptor validation", "[pam4-wide][typed][negative]")
{
    auto fixture = MakeFixture();
    const std::array<std::byte, 9> invalidDescriptor{};
    fixture.blocks[0] = Wrap(pbprotocol::DeriveSessionTag(fixture.session.sessionId), pbprotocol::ControlRecordType::SessionDescriptor, invalidDescriptor);
    const auto slots = Slots(fixture);
    REQUIRE(EncodeExperimentalPam4WideFrame({fixture.bootstrap, slots}, fixture.raster));
    auto decoder = MakeDecoder();
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    auto receiver = pbprotocol::ControlPlaneReceiver::Create(pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(receiver);
    const auto& block = decoder.GetAcceptedBlocks().front();
    CHECK_FALSE(receiver.Value().ReceiveControlRecord(std::span(block.bytes).first(block.size)));
    CHECK(receiver.Value().ActiveSessionCount() == 0);
}

TEST_CASE("PAM4 Wide typed Segment and FinalManifest share the same Session over pixels", "[pam4-wide][typed]")
{
    auto fixture = MakeFixture(3267, 1024);
    auto decoder = MakeDecoder();
    auto receiver = pbprotocol::ControlPlaneReceiver::Create(pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(receiver);
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    const auto AdmitControl = [&decoder, &receiver]()
    {
        const auto& block = decoder.GetAcceptedBlocks().front();
        return receiver.Value().ReceiveControlRecord(std::span(block.bytes).first(block.size));
    };
    REQUIRE(AdmitControl());
    const auto tag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    pbprotocol::SegmentDescriptor segment;
    segment.sessionTag = tag;
    segment.rawSize = fixture.session.originalFileSize;
    segment.encodedSize = segment.rawSize;
    segment.compressionCodec = pbprotocol::CompressionCodec::Raw;
    segment.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    segment.outerBlockBytes = 1629;
    const auto segmentSize = pbprotocol::GetSerializedSize(segment);
    REQUIRE(segmentSize);
    std::vector<std::byte> payload(segmentSize.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(segment, fixture.session, pbprotocol::GetDefaultReceiverResourcePolicy(), payload));
    fixture.blocks[0] = Wrap(tag, pbprotocol::ControlRecordType::SegmentDescriptor, payload, 8);
    auto bootstrap = pbprotocol::ParseBootstrapRecord(fixture.bootstrap).Value();
    bootstrap.frameSequence++;
    REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, fixture.bootstrap));
    auto slots = Slots(fixture);
    slots[0].assignment.controlPriority = UnifiedControlPriority::CurrentSegmentDescriptor;
    REQUIRE(EncodeExperimentalPam4WideFrame({fixture.bootstrap, slots}, fixture.raster));
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    REQUIRE(AdmitControl());
    const auto bound = receiver.Value().GetBoundSegmentDescriptor(tag, 0);
    REQUIRE(bound);
    REQUIRE(receiver.Value().BoundSegmentCount(tag).Value() == 1);
    pbprotocol::FinalManifest manifest;
    manifest.sessionId = fixture.session.sessionId;
    manifest.originalFileSize = fixture.session.originalFileSize;
    manifest.segmentCount = 1;
    manifest.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
    payload.resize(pbprotocol::kFinalManifestPayloadBytes);
    REQUIRE(pbprotocol::SerializeFinalManifest(manifest, fixture.session, pbprotocol::GetDefaultReceiverResourcePolicy(), payload));
    fixture.blocks[0] = Wrap(tag, pbprotocol::ControlRecordType::FinalManifest, payload, 9);
    bootstrap.frameSequence++;
    REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, fixture.bootstrap));
    slots = Slots(fixture);
    slots[0].assignment.controlPriority = UnifiedControlPriority::FinalManifest;
    REQUIRE(EncodeExperimentalPam4WideFrame({fixture.bootstrap, slots}, fixture.raster));
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    REQUIRE(AdmitControl());
    CHECK(receiver.Value().HasFinalManifest(tag).Value());
    // A valid manifest must not turn unreceived/unverified file data into a
    // completed Segment or a publishable file.
    CHECK_FALSE(receiver.Value().IsSegmentCompleted(tag, 0).Value());
    CHECK_FALSE(receiver.Value().PrepareFinalization(tag));
}

TEST_CASE("PAM4 Wide typed ingress retains the 64-block DirectRepeat resource ceiling", "[pam4-wide][typed][resources]")
{
    auto fixture = MakeFixture();
    auto decoder = MakeDecoder();
    auto receiver = pbprotocol::ControlPlaneReceiver::Create(pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(receiver);
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    const auto control = decoder.GetAcceptedBlocks().front();
    REQUIRE(receiver.Value().ReceiveControlRecord(std::span(control.bytes).first(control.size)));
    pbprotocol::SegmentDescriptor segment;
    segment.sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    segment.rawSize = fixture.session.originalFileSize;
    segment.encodedSize = segment.rawSize;
    segment.compressionCodec = pbprotocol::CompressionCodec::Raw;
    segment.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    segment.outerBlockBytes = 1629;
    const auto size = pbprotocol::GetSerializedSize(segment);
    REQUIRE(size);
    std::vector<std::byte> payload(size.Value());
    // Valid wire shape, deliberately invalid receiver resource demand. This
    // does not change or disable the production policy during receive.
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(segment, fixture.session, payload));
    const auto status = pbprotocol::ValidateSegmentDescriptor(segment, fixture.session, pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE_FALSE(status);
    CHECK(status.Error().code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    fixture.blocks[0] = Wrap(segment.sessionTag, pbprotocol::ControlRecordType::SegmentDescriptor, payload, 8);
    auto bootstrap = pbprotocol::ParseBootstrapRecord(fixture.bootstrap).Value();
    bootstrap.frameSequence++;
    REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, fixture.bootstrap));
    auto slots = Slots(fixture);
    slots[0].assignment.controlPriority = UnifiedControlPriority::CurrentSegmentDescriptor;
    REQUIRE(EncodeExperimentalPam4WideFrame({fixture.bootstrap, slots}, fixture.raster));
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    const auto block = decoder.GetAcceptedBlocks().front();
    const auto admission = receiver.Value().ReceiveControlRecord(std::span(block.bytes).first(block.size));
    CHECK_FALSE(admission);
    CHECK(admission.Error().code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    CHECK(receiver.Value().BoundSegmentCount(segment.sessionTag).Value() == 0);
}

TEST_CASE("PAM4 Wide fixed slot plan rejects ambiguous or unknown assignments transactionally", "[pam4-wide][negative]")
{
    const auto fixture = MakeFixture();
    const auto valid = Slots(fixture);
    for (std::uint32_t fault = 0; fault < 8; fault++)
    {
        auto slots = valid;
        switch (fault)
        {
        case 0: slots[1].assignment.codewordSlot = 0; break;
        case 1: slots[1].assignment.codewordSlot = 20; break;
        case 2: slots[1].assignment.kind = static_cast<UnifiedSlotKind>(255); break;
        case 3: slots[1].assignment.controlPriority = UnifiedControlPriority::SessionDescriptor; break;
        case 4: slots[0].assignment.controlPriority = UnifiedControlPriority::NotApplicable; break;
        case 5: slots[0].assignment.controlPriority = UnifiedControlPriority::FinalManifest; break;
        case 6: slots[0].active = false; break;
        case 7: slots[1].active = false; break;
        }
        std::array<std::byte, kExperimentalPam4WideCodedFrameBytes> output;
        output.fill(std::byte{0xA5});
        CHECK_FALSE(PackExperimentalPam4WideFrame({fixture.bootstrap, slots}, output));
        CHECK(std::ranges::all_of(output, [](const auto value) { return value == std::byte{0xA5}; }));
    }
    CHECK_FALSE(PackExperimentalPam4WideFrame({fixture.bootstrap, std::span(valid).first(19)}, {}));
    std::array<UnifiedSlotAssignment, 20> assignments{};
    for (std::size_t index = 0; index < assignments.size(); index++)
    {
        assignments[index] = valid[index].assignment;
    }
    CHECK(ValidateExperimentalPam4WideSlotPlan(assignments));
    CHECK_FALSE(ValidateUnifiedGrayMixedSlotPlan(assignments));
    CHECK_FALSE(ValidateUnifiedMixedSlotPlan(assignments));
    CHECK_FALSE(ValidateExperimentalPam4SlotPlan(assignments));
    for (const std::size_t count : {11U, 18U, 19U})
    {
        std::array<std::byte, kExperimentalPam4WideCodedFrameBytes> output;
        output.fill(std::byte{0xA5});
        CHECK_FALSE(PackExperimentalPam4WideFrame({fixture.bootstrap, std::span(valid).first(count)}, output));
        CHECK(std::ranges::all_of(output, [](const auto value) { return value == std::byte{0xA5}; }));
    }
}

TEST_CASE("PAM4 Wide uses fixed Robust Control and Fast Data dimensions with zero fillers", "[pam4-wide][fec]")
{
    const auto fixture = MakeFixture();
    for (std::uint32_t slot = 0; slot < 20; slot++)
    {
        const auto codeword = std::span(fixture.coded).subspan(slot * 2025, 2025);
        const auto syndrome = pbinnerfec::ComputeQcLdpcSyndrome(slot == 0 ? pbinnerfec::kInnerFecProfileIdRobust : pbinnerfec::kInnerFecProfileIdFast, codeword);
        REQUIRE(syndrome);
        CHECK(syndrome.Value());
    }
    auto slots = Slots(fixture);
    for (std::uint32_t slot = 1; slot < 20; slot++)
    {
        slots[slot].active = false;
        slots[slot].block = {};
    }
    std::vector<std::byte> raster(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeExperimentalPam4WideFrame({fixture.bootstrap, slots}, raster));
    auto decoder = MakeDecoder();
    const auto observation = decoder.Decode(View(raster));
    REQUIRE(observation.IsFrameAvailable());
    CHECK(observation.acceptedBlocks == 1);
    CHECK(observation.acceptedTransportBlocks == 0);
    for (std::uint32_t slot = 1; slot < 20; slot++)
    {
        CHECK(observation.slotObservations[slot].fecValid);
        CHECK_FALSE(observation.slotObservations[slot].accepted);
    }
}

TEST_CASE("PAM4 Wide exact independent identity rejects old and mixed bindings", "[pam4-wide][identity]")
{
    const auto fixture = MakeFixture();
    auto decoder = MakeDecoder();
    const std::array identities{pbprotocol::kUnifiedProductVisualProfile, pbprotocol::kBlankControlExperimentalProfile,
        pbprotocol::kGrayStatesExperimentalProfile, pbprotocol::kGrayFastExperimentalProfile, pbprotocol::kPam4ExperimentalProfile};
    for (const auto identity : identities)
    {
        auto bootstrap = pbprotocol::ParseBootstrapRecord(fixture.bootstrap).Value();
        bootstrap.visualProfileId = identity.visualProfileId;
        bootstrap.visualLayoutVersion = identity.visualLayoutVersion;
        std::array<std::byte, 44> bytes{};
        REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, bytes));
        std::vector<std::byte> unchanged(kUnifiedFrameBgraBytes, std::byte{0xA5});
        CHECK_FALSE(EncodeExperimentalPam4WideFrame(bytes, fixture.coded, unchanged));
        CHECK(std::ranges::all_of(unchanged, [](const auto value) { return value == std::byte{0xA5}; }));
        CHECK_FALSE(DecodeLocalDesktopBootstrap(View(fixture.raster), LocalDesktopBootstrapBinding{identity.visualProfileId, identity.visualLayoutVersion}).IsAccepted());
        auto expected = kExperimentalPam4WideExpectedIdentity;
        expected.visualProfileId = identity.visualProfileId;
        CHECK_FALSE(decoder.Decode(View(fixture.raster), expected).IsFrameAvailable());
    }
    auto expected = kExperimentalPam4WideExpectedIdentity;
    expected.requireSessionTag = true;
    expected.sessionTag = {123};
    CHECK_FALSE(decoder.Decode(View(fixture.raster), expected).IsFrameAvailable());
    expected = kExperimentalPam4WideExpectedIdentity;
    expected.requireFrameSequence = true;
    expected.frameSequence = 0;
    CHECK_FALSE(decoder.Decode(View(fixture.raster), expected).IsFrameAvailable());
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    auto oldDecoder = ExperimentalPam4CpuDecoder::Create(ExperimentalPam4CpuDecoder::RequiredBytes());
    REQUIRE(oldDecoder);
    CHECK_FALSE(oldDecoder.Value().Decode(View(fixture.raster)).IsFrameAvailable());
    CHECK(oldDecoder.Value().GetAcceptedBlocks().empty());
    auto wrongLayout = pbprotocol::ParseBootstrapRecord(fixture.bootstrap).Value();
    wrongLayout.visualLayoutVersion = pbprotocol::kPam4ExperimentalProfile.visualLayoutVersion;
    std::array<std::byte, 44> wrongLayoutBytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(wrongLayout, wrongLayoutBytes));
    std::vector<std::byte> unchanged(kUnifiedFrameBgraBytes, std::byte{0xA5});
    CHECK_FALSE(EncodeExperimentalPam4WideFrame(wrongLayoutBytes, fixture.coded, unchanged));
    CHECK(std::ranges::all_of(unchanged, [](const auto value) { return value == std::byte{0xA5}; }));
}

TEST_CASE("PAM4 Wide CRC padding and SessionTag remain mandatory after valid FEC", "[pam4-wide][negative][fec]")
{
    const auto fixture = MakeFixture();
    auto decoder = MakeDecoder();
    for (std::uint32_t fault = 0; fault < 4; fault++)
    {
        const std::uint32_t slot = fault / 2;
        const auto informationBytes = slot == 0 ? 1350U : 1665U;
        auto coded = fixture.coded;
        std::vector<std::byte> information(informationBytes);
        std::copy_n(fixture.coded.begin() + slot * 2025, informationBytes, information.begin());
        const std::size_t offset = fault % 2 == 0 ? 30 : informationBytes - 1;
        information[offset] ^= std::byte{1};
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(slot == 0 ? pbinnerfec::kInnerFecProfileIdRobust : pbinnerfec::kInnerFecProfileIdFast,
            information, std::span(coded).subspan(slot * 2025, 2025)));
        std::vector<std::byte> raster(kUnifiedFrameBgraBytes);
        REQUIRE(EncodeExperimentalPam4WideFrame(fixture.bootstrap, coded, raster));
        const auto observation = decoder.Decode(View(raster));
        REQUIRE(observation.IsFrameAvailable());
        CHECK(observation.slotObservations[slot].fecValid);
        CHECK_FALSE(observation.slotObservations[slot].accepted);
        CHECK(observation.acceptedBlocks == 19);
    }
    const auto parsed = pbprotocol::ParseTransportBlock(fixture.blocks[1]);
    REQUIRE(parsed);
    auto wrongHeader = parsed.Value().header;
    wrongHeader.sessionTag.value ^= 1;
    std::vector<std::byte> wrongBlock(fixture.blocks[1].size());
    REQUIRE(pbprotocol::SerializeTransportBlock(wrongHeader, parsed.Value().payload, wrongBlock));
    std::array<std::byte, 1665> information{};
    REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(wrongBlock, information.size(), information));
    auto coded = fixture.coded;
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdFast, information, std::span(coded).subspan(2025, 2025)));
    std::vector<std::byte> raster(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeExperimentalPam4WideFrame(fixture.bootstrap, coded, raster));
    const auto observation = decoder.Decode(View(raster));
    REQUIRE(observation.IsFrameAvailable());
    CHECK(observation.slotObservations[1].fecValid);
    CHECK(observation.slotObservations[1].crcValid);
    CHECK_FALSE(observation.slotObservations[1].accepted);
    CHECK(observation.slotObservations[1].rejection == UnifiedSlotRejection::IdentityFailure);
    auto wrongFixture = fixture;
    wrongFixture.blocks[1] = wrongBlock;
    const auto wrongSlots = Slots(wrongFixture);
    CHECK_FALSE(PackExperimentalPam4WideFrame({wrongFixture.bootstrap, wrongSlots}, coded));
}

TEST_CASE("PAM4 Wide partial freshness and calibration erasures clear cached state", "[pam4-wide][negative][cache]")
{
    const auto fixture = MakeFixture();
    const auto next = MakeFixture(3268);
    auto decoder = MakeDecoder();
    Exact(fixture, decoder.Decode(View(fixture.raster)), decoder);
    CHECK(decoder.Decode(View(fixture.raster)).bootstrap.windowedFastPath);
    for (std::size_t index = 5; index < 15; index++)
    {
        auto damaged = fixture.raster;
        CopyRegion(next.raster, damaged, kUnifiedVisualProfile.regions[index].bounds);
        CHECK_FALSE(decoder.Decode(View(damaged)).IsFrameAvailable());
        CHECK(decoder.GetAcceptedBlocks().empty());
        CHECK(decoder.GetSoftMetrics().empty());
        const auto reacquired = decoder.Decode(View(fixture.raster));
        Exact(fixture, reacquired, decoder);
        CHECK_FALSE(reacquired.bootstrap.windowedFastPath);
    }
    auto damaged = fixture.raster;
    const std::vector<std::byte> blank(damaged.size(), std::byte{128});
    for (std::size_t index = 15; index < 19; index++)
    {
        CopyRegion(blank, damaged, kUnifiedVisualProfile.regions[index].bounds);
    }
    CHECK_FALSE(decoder.Decode(View(damaged)).IsFrameAvailable());
    CHECK_FALSE(decoder.GetCalibration().valid);
    Exact(next, decoder.Decode(View(next.raster)), decoder);
    decoder.Reset();
    CHECK(decoder.GetAcceptedBlocks().empty());
    CHECK_FALSE(decoder.Decode(View(next.raster)).bootstrap.windowedFastPath);
}

TEST_CASE("PAM4 Wide supports padded Gray8 and neutral BGRA without format inference", "[pam4-wide][stride]")
{
    const auto fixture = MakeFixture();
    auto decoder = MakeDecoder();
    constexpr std::size_t pitch = 1920 + 31;
    std::vector<std::byte> gray(pitch * 1080, std::byte{0xA5});
    for (std::size_t row = 0; row < 1080; row++)
    {
        for (std::size_t column = 0; column < 1920; column++)
        {
            gray[row * pitch + column] = fixture.raster[(row * 1920 + column) * 4];
        }
    }
    Exact(fixture, decoder.Decode({gray, 1920, 1080, pitch, LumaPixelFormat::Gray8}), decoder);
    CHECK_FALSE(decoder.Decode({gray, 1920, 1080, 1919, LumaPixelFormat::Gray8}).IsFrameAvailable());
    CHECK_FALSE(decoder.Decode({std::span(gray).first(1080), 1920, 1080, pitch, LumaPixelFormat::Gray8}).IsFrameAvailable());
    CHECK_FALSE(decoder.Decode({gray, UINT32_MAX, 1080, pitch, LumaPixelFormat::Gray8}).IsFrameAvailable());
    CHECK_FALSE(decoder.Decode({}).IsFrameAvailable());
    CHECK(decoder.GetSoftMetrics().empty());
    CHECK_FALSE(ExperimentalPam4WideCpuDecoder::Create(0));
    CHECK_FALSE(ExperimentalPam4WideCpuDecoder::Create(ExperimentalPam4WideCpuDecoder::RequiredBytes() - 1));
    CHECK(ExperimentalPam4WideCpuDecoder::RequiredBytes() < 4U * 1024U * 1024U);
    ExperimentalPam4WideCpuDecoder empty;
    CHECK_FALSE(empty.Decode(View(fixture.raster)).IsFrameAvailable());
    auto moved = std::move(decoder);
    CHECK_FALSE(decoder.Decode(View(fixture.raster)).IsFrameAvailable());
    Exact(fixture, moved.Decode(View(fixture.raster)), moved);
}

TEST_CASE("PAM4 Wide fixed 4/3 presentation and fractional remote capture reacquire from pixels", "[pam4-wide][scale]")
{
    auto decoder = MakeDecoder();
    for (const auto sequence : {3267ULL, 3268ULL, 3284ULL})
    {
        const auto fixture = MakeFixture(sequence);
        for (const std::uint32_t offset : {0U, 1U})
        {
            const auto captured = FixedWidePresentationCapture(fixture.raster, offset);
            const auto observation = decoder.Decode({captured, 2560, 1440, 2560 * 4, LumaPixelFormat::Bgra8});
            Exact(fixture, observation, decoder);
            CHECK(std::abs(observation.bootstrap.geometry.scaleX - 0.851 * 4 / 3) * 1920 <= 1.25);
        }
    }
}

TEST_CASE("PAM4 Wide geometry movement misses the cache and reacquires without external hints", "[pam4-wide][cache]")
{
    const auto fixture = MakeFixture();
    auto captured = pbtest::MakeUnifiedGrayResampledFixture(fixture.raster, 1000, false, 0);
    auto decoder = MakeDecoder();
    Exact(fixture, decoder.Decode({captured, 2560, 1440, 2560 * 4, LumaPixelFormat::Bgra8}), decoder);
    std::vector<std::byte> moved(captured.size());
    for (std::size_t row = 0; row < 1440; row++)
    {
        std::copy_n(captured.begin() + static_cast<std::ptrdiff_t>(row * 2560 * 4), (2560 - 200) * 4,
            moved.begin() + static_cast<std::ptrdiff_t>((row * 2560 + 200) * 4));
    }
    const auto reacquired = decoder.Decode({moved, 2560, 1440, 2560 * 4, LumaPixelFormat::Bgra8});
    Exact(fixture, reacquired, decoder);
    CHECK_FALSE(reacquired.bootstrap.windowedFastPath);
    CHECK(decoder.Decode({moved, 2560, 1440, 2560 * 4, LumaPixelFormat::Bgra8}).bootstrap.windowedFastPath);
}

TEST_CASE("PAM4 Wide preserves the existing non-Data reserved raster except identity-bound bits", "[pam4-wide][raster][golden]")
{
    const auto fixture = MakeFixture();
    auto oldBootstrap = pbprotocol::ParseBootstrapRecord(fixture.bootstrap).Value();
    oldBootstrap.visualProfileId = pbprotocol::kGrayFastExperimentalProfile.visualProfileId;
    oldBootstrap.visualLayoutVersion = pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion;
    std::array<std::byte, 44> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(oldBootstrap, bytes));
    const std::vector<std::byte> oldCoded(kUnifiedGrayCodedFrameBytes);
    std::vector<std::byte> oldRaster(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeUnifiedVisualFrame(bytes, oldCoded, oldRaster));
    std::vector<bool> excluded(1920 * 1080);
    for (const auto& region : kUnifiedVisualProfile.regions)
    {
        if (region.kind != UnifiedRegionKind::Data && region.kind != UnifiedRegionKind::BootstrapA && region.kind != UnifiedRegionKind::BootstrapB &&
            !HasUnifiedPilotContent(region.pilotContent, UnifiedPilotContent::TimingFreshness))
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.bounds.height; row++)
        {
            for (std::uint32_t column = 0; column < region.bounds.width; column++)
            {
                excluded[(region.bounds.y + row) * 1920 + region.bounds.x + column] = true;
            }
        }
    }
    std::size_t compared = 0;
    bool equal = true;
    for (std::size_t pixel = 0; pixel < excluded.size(); pixel++)
    {
        if (!excluded[pixel])
        {
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                equal = equal && fixture.raster[pixel * 4 + channel] == oldRaster[pixel * 4 + channel];
            }
            compared++;
        }
    }
    CHECK(compared > 200000);
    CHECK(equal);
    auto shortOutput = std::vector<std::byte>(kUnifiedFrameBgraBytes - 1, std::byte{0xA5});
    CHECK_FALSE(EncodeExperimentalPam4WideFrame(fixture.bootstrap, fixture.coded, shortOutput));
    CHECK(std::ranges::all_of(shortOutput, [](const auto value) { return value == std::byte{0xA5}; }));
    auto overlap = fixture.raster;
    const auto before = overlap;
    CHECK_FALSE(EncodeExperimentalPam4WideFrame(fixture.bootstrap, std::span(overlap).first(kExperimentalPam4WideCodedFrameBytes), overlap));
    CHECK(overlap == before);
}
