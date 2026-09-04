#include "protocol_dump_core.h"

#include "descriptor_test_helpers.h"
#include "pbgolden/golden_vector_source.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::byte> SerializeSession(const pbprotocol::SessionDescriptor& descriptor)
{
    const auto size = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(descriptor, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionContext(const std::uint64_t fileSize)
{
    return SerializeSession(pbprotocol::test::MakeSessionDescriptor(fileSize, 1));
}

[[nodiscard]] std::vector<std::byte> MakeSegmentPayload(const pbprotocol::SessionDescriptor& session,
    const bool wirehair = false)
{
    const auto descriptor = wirehair ? pbprotocol::test::MakeWirehairSegment(session, 0, 0, session.originalFileSize) :
        pbprotocol::test::MakeDirectRepeatSegment(session, 0, 0, session.originalFileSize);
    const auto size = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(descriptor, session, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeManifestPayload(const pbprotocol::SessionDescriptor& session)
{
    const auto manifest = pbprotocol::test::MakeFinalManifest(session);
    std::vector<std::byte> bytes(pbprotocol::GetSerializedSize(manifest));
    REQUIRE(pbprotocol::SerializeFinalManifest(manifest, session, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeControlRecord(const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, 1,
        pbprotocol::DeriveSessionTag(pbprotocol::test::MakeSessionId()), payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

void WriteUint32(const std::span<std::byte> bytes, const std::size_t offset, const std::uint32_t value)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(4 <= bytes.size() - offset);
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8))};
    }
}

void RewriteDescriptorCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() >= 4);
    const std::size_t offset = bytes.size() - 4;
    WriteUint32(bytes, offset, pbprotocol::ComputeCrc32c(std::span<const std::byte>(bytes).first(offset)));
}

void RequireField(const pbdump::DumpReport& report, const std::string& name,
    const std::size_t offset, const std::size_t size, const std::string& value = {})
{
    const auto field = std::find_if(report.fields.begin(), report.fields.end(),
        [&](const pbdump::FieldRow& candidate)
        {
            return candidate.name == name;
        });
    INFO(name);
    REQUIRE(field != report.fields.end());
    REQUIRE(field->offset == offset);
    REQUIRE(field->size == size);
    if (!value.empty())
    {
        REQUIRE(field->value == value);
    }
}

void RequireNoField(const pbdump::DumpReport& report, const std::string& name)
{
    REQUIRE(std::none_of(report.fields.begin(), report.fields.end(),
        [&](const pbdump::FieldRow& field)
        {
            return field.name == name;
        }));
}

void RequireSuccess(const std::vector<std::byte>& bytes, const std::string& type,
    const std::span<const std::byte> context = {})
{
    const auto report = pbdump::DumpRecord(bytes, type, context);
    REQUIRE(report.type == type);
    REQUIRE(report.parseOk);
    REQUIRE_FALSE(report.parseSkipped);
    REQUIRE_FALSE(report.hasDiagnostic);
    const std::string formatted = pbdump::FormatDump(report);
    REQUIRE(formatted.find("[parse] status=Success offset=0") != std::string::npos);
}

} // namespace

TEST_CASE("PBProtocolDump accepts every supported record type with formal descriptors", "[tools][protocol-dump]")
{
    const auto session117 = MakeSessionContext(117);
    const auto session200 = MakeSessionContext(200);
    RequireSuccess(pbgolden::GenerateBootstrapRecord(), "bootstrap");
    RequireSuccess(MakeControlRecord(session117), "control");
    RequireSuccess(pbgolden::GenerateControlFragment(0), "fragment");
    RequireSuccess(session117, "session-descriptor");
    RequireSuccess(MakeSegmentPayload(pbprotocol::test::MakeSessionDescriptor(117, 1)),
        "segment-descriptor", session117);
    RequireSuccess(MakeSegmentPayload(pbprotocol::test::MakeSessionDescriptor(200, 1), true),
        "segment-descriptor", session200);
    RequireSuccess(MakeManifestPayload(pbprotocol::test::MakeSessionDescriptor(117, 1)), "final-manifest", session117);
    RequireSuccess(pbgolden::GenerateTransportBlockCanonical(), "transport");
    RequireSuccess(pbgolden::GenerateWirehairCanonicalDescriptor(), "wirehair-descriptor");
    RequireSuccess(pbgolden::GenerateReferenceRasterManifest(), "pbvm-manifest");
}

TEST_CASE("PBProtocolDump auto detection is magic-only plus strict Transport",
    "[tools][protocol-dump]")
{
    const auto bootstrap = pbdump::DumpRecord(pbgolden::GenerateBootstrapRecord(), "auto", {});
    REQUIRE(bootstrap.type == "bootstrap");
    const auto transport = pbdump::DumpRecord(
        pbgolden::GenerateTransportBlockMinimum(), "auto", {});
    REQUIRE(transport.type == "transport");
    const auto descriptor = pbdump::DumpRecord(
        MakeSessionContext(117), "auto", {});
    REQUIRE(descriptor.type == "unrecognized");
    REQUIRE_FALSE(descriptor.parseOk);
}

TEST_CASE("PBProtocolDump reports byte offset expected and actual", "[tools][protocol-dump]")
{
    auto bootstrap = pbgolden::GenerateBootstrapRecord();
    bootstrap[40] ^= std::byte{1};
    const auto crcReport = pbdump::DumpRecord(bootstrap, "bootstrap", {});
    REQUIRE_FALSE(crcReport.parseOk);
    REQUIRE(crcReport.hasDiagnostic);
    REQUIRE(crcReport.diagnosticOffset == 40);
    REQUIRE(crcReport.diagnosticExpected != crcReport.diagnosticActual);
    REQUIRE(pbdump::FormatDump(crcReport).find(
        "[diagnostic] space=record byte_offset=40 expected=0x") != std::string::npos);

    bootstrap = pbgolden::GenerateBootstrapRecord();
    bootstrap.pop_back();
    const auto truncated = pbdump::DumpRecord(bootstrap, "bootstrap", {});
    REQUIRE(truncated.parseCode == pbprotocol::ProtocolErrorCode::TruncatedInput);
    REQUIRE(truncated.diagnosticOffset == 43);
    REQUIRE(truncated.diagnosticActual == "<eof>");

    bootstrap = pbgolden::GenerateBootstrapRecord();
    bootstrap.push_back(std::byte{0xA5});
    const auto trailing = pbdump::DumpRecord(bootstrap, "bootstrap", {});
    REQUIRE(trailing.parseCode == pbprotocol::ProtocolErrorCode::TrailingBytes);
    REQUIRE(trailing.diagnosticOffset == 44);
    REQUIRE(trailing.diagnosticExpected == "<eof>");
    REQUIRE(trailing.diagnosticActual == "0xa5");

    // Transport deliberately validates type/minor/flags/reserved before its
    // header CRC. A stale CRC must not replace the authoritative semantic
    // branch in the common failure diagnostic.
    auto transport = pbgolden::GenerateTransportBlockCanonical();
    transport[0] = std::byte{2};
    const auto semantic = pbdump::DumpRecord(transport, "transport", {});
    REQUIRE(semantic.parseCode ==
        pbprotocol::ProtocolErrorCode::InvalidEnumValue);
    REQUIRE(semantic.crcGates.size() == 2);
    REQUIRE_FALSE(semantic.crcGates[0].Matches());
    REQUIRE(semantic.diagnosticOffset == 0);
    REQUIRE(semantic.diagnosticExpected == "valid-InvalidEnumValue");
    REQUIRE(semantic.diagnosticActual == "0x02");

}

TEST_CASE("PBProtocolDump derives variable fields from declared boundaries",
    "[tools][protocol-dump]")
{
    auto control = pbgolden::GenerateControlSessionDescriptor();
    control.push_back(std::byte{0xEE});
    const auto report = pbdump::DumpRecord(control, "control", {});
    REQUIRE(report.parseCode == pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    REQUIRE(report.diagnosticOffset == 67);
    REQUIRE(report.diagnosticExpected == "<eof>");
    REQUIRE(report.diagnosticActual == "0xee");
    REQUIRE(report.crcGates.size() == 1);
    REQUIRE(report.crcGates[0].offset == 63);
    REQUIRE(report.crcGates[0].Matches());
    const auto crcField = std::find_if(report.fields.begin(), report.fields.end(),
        [](const pbdump::FieldRow& field) { return field.name == "Crc32c"; });
    REQUIRE(crcField != report.fields.end());
    REQUIRE(crcField->offset == 63);

    auto fragment = pbgolden::GenerateControlFragment(2);
    fragment.push_back(std::byte{0xEE});
    const auto fragmentReport = pbdump::DumpRecord(fragment, "fragment", {});
    REQUIRE(fragmentReport.crcGates.size() == 1);
    REQUIRE(fragmentReport.crcGates[0].offset == 39);
    REQUIRE(fragmentReport.crcGates[0].Matches());

    // A declared record boundary that is not fully present must not produce
    // a payload preview or a CRC row from the actual file tail.
    control = pbgolden::GenerateControlSessionDescriptor();
    control.resize(40);
    const auto truncatedControl = pbdump::DumpRecord(control, "control", {});
    REQUIRE_FALSE(truncatedControl.parseOk);
    REQUIRE(truncatedControl.crcGates.empty());
    REQUIRE(std::none_of(truncatedControl.fields.begin(),
        truncatedControl.fields.end(), [](const pbdump::FieldRow& field)
        {
            return field.name == "Payload" || field.name == "Crc32c";
        }));

    fragment = pbgolden::GenerateControlFragment(0);
    fragment.resize(30);
    const auto truncatedFragment = pbdump::DumpRecord(
        fragment, "fragment", {});
    REQUIRE_FALSE(truncatedFragment.parseOk);
    REQUIRE(truncatedFragment.crcGates.empty());
    REQUIRE(std::none_of(truncatedFragment.fields.begin(),
        truncatedFragment.fields.end(), [](const pbdump::FieldRow& field)
        {
            return field.name == "Payload" || field.name == "Crc32c";
        }));
}

TEST_CASE("PBProtocolDump context-dependent semantics fail closed", "[tools][protocol-dump]")
{
    const auto segment = MakeSegmentPayload(pbprotocol::test::MakeSessionDescriptor(117, 1));
    const auto missing = pbdump::DumpRecord(segment, "segment-descriptor", {});
    REQUIRE(missing.parseSkipped);
    REQUIRE(missing.contextStatus == "skipped");
    REQUIRE(missing.hasDiagnostic);
    REQUIRE(missing.diagnosticSpace == "context");
    REQUIRE(missing.diagnosticExpected == "formal-schema-1-session-descriptor");
    REQUIRE(missing.diagnosticActual == "missing");

    auto invalidContext = MakeSessionContext(117);
    invalidContext[8] = std::byte{2};
    RewriteDescriptorCrc(invalidContext);
    const auto invalid = pbdump::DumpRecord(
        segment, "segment-descriptor", invalidContext);
    REQUIRE(invalid.parseSkipped);
    REQUIRE(invalid.contextStatus == "invalid-session-descriptor");
    REQUIRE(invalid.diagnosticSpace == "context");
    REQUIRE(invalid.diagnosticOffset == 8);
    REQUIRE(invalid.diagnosticExpected == "valid-UnsupportedProtocolMajor");
    REQUIRE(invalid.diagnosticActual == "0x02");

    auto foreignSession = pbprotocol::test::MakeSessionDescriptor(117, 1);
    foreignSession.sessionId.bytes[0] ^= std::byte{1};
    const auto wrongContext = SerializeSession(foreignSession);
    const auto mismatch = pbdump::DumpRecord(segment, "segment-descriptor", wrongContext);
    REQUIRE_FALSE(mismatch.parseOk);
    REQUIRE_FALSE(mismatch.parseSkipped);
    REQUIRE(mismatch.parseCode == pbprotocol::ProtocolErrorCode::SessionTagMismatch);
}

TEST_CASE("PBProtocolDump field rows and CRC gates match the formal descriptor offsets",
    "[tools][protocol-dump][descriptor-schema]")
{
    const auto session = pbprotocol::test::MakeSessionDescriptor(117, 1);
    const auto context = SerializeSession(session);
    const auto sessionReport = pbdump::DumpRecord(context, "session-descriptor", {});
    REQUIRE(sessionReport.parseOk);
    RequireField(sessionReport, "DescriptorSchemaVersion", 0, 2, "0001");
    RequireField(sessionReport, "DescriptorHeaderBytes", 2, 2, "0046");
    RequireField(sessionReport, "DescriptorTotalBytes", 4, 4, "00000055");
    RequireField(sessionReport, "ProtocolMajor", 8, 2, "0001");
    RequireField(sessionReport, "ProtocolMinor", 10, 2, "0000");
    RequireField(sessionReport, "SessionId", 12, 16, "000102030405060708090a0b0c0d0e0f");
    RequireField(sessionReport, "SessionVisualProfileId", 28, 8, "5042554e494c4331");
    RequireField(sessionReport, "FileSize", 36, 8, "0000000000000075");
    RequireField(sessionReport, "SourceSegmentTargetBytes", 44, 4, "00800000");
    RequireField(sessionReport, "SegmentCount", 48, 8, "0000000000000001");
    RequireField(sessionReport, "CompressionPolicy", 56, 1, "01");
    RequireField(sessionReport, "DigestAlgorithm", 57, 1, "01");
    RequireField(sessionReport, "Reserved", 58, 2, "0000");
    RequireField(sessionReport, "FeatureFlags", 60, 8, "0000000000000000");
    RequireField(sessionReport, "FileNameUtf8Bytes", 68, 2, "000b");
    RequireField(sessionReport, "FileNameUtf8", 70, 11, "7061796c6f61642e62696e");
    RequireField(sessionReport, "DescriptorCrc32c", 81, 4);
    RequireNoField(sessionReport, "OptionalExtensions");
    REQUIRE(sessionReport.crcGates.size() == 1);
    REQUIRE(sessionReport.crcGates[0].offset == 81);
    REQUIRE(sessionReport.crcGates[0].Matches());

    const auto direct = pbdump::DumpRecord(MakeSegmentPayload(session), "segment-descriptor", context);
    REQUIRE(direct.parseOk);
    RequireField(direct, "DescriptorHeaderBytes", 2, 2, "0080");
    RequireField(direct, "DescriptorTotalBytes", 4, 4, "00000084");
    RequireField(direct, "SessionTag", 8, 8, "81df204bd997bad0");
    RequireField(direct, "SegmentOrdinal", 16, 8, "0000000000000000");
    RequireField(direct, "RawOffset", 24, 8, "0000000000000000");
    RequireField(direct, "RawSize", 32, 8, "0000000000000075");
    RequireField(direct, "EncodedSize", 40, 8, "0000000000000075");
    RequireField(direct, "CompressionCodec", 48, 1, "01");
    RequireField(direct, "OuterFecMode", 49, 1, "02");
    RequireField(direct, "Reserved", 50, 2, "0000");
    RequireField(direct, "OuterBlockBytes", 52, 4, "00000010");
    RequireField(direct, "RawDigest", 56, 32, "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f");
    RequireField(direct, "EncodedDigest", 88, 32, "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f");
    RequireField(direct, "Flags", 120, 8, "0000000000000000");
    RequireField(direct, "DescriptorCrc32c", 128, 4);
    RequireNoField(direct, "WirehairProfile");
    REQUIRE(direct.crcGates.size() == 1);
    REQUIRE(direct.crcGates[0].Matches());

    const auto wirehairSession = pbprotocol::test::MakeSessionDescriptor(200, 1);
    const auto wirehair = pbdump::DumpRecord(MakeSegmentPayload(wirehairSession, true),
        "segment-descriptor", SerializeSession(wirehairSession));
    REQUIRE(wirehair.parseOk);
    RequireField(wirehair, "DescriptorTotalBytes", 4, 4, "000000a4");
    RequireField(wirehair, "RawSize", 32, 8, "00000000000000c8");
    RequireField(wirehair, "OuterFecMode", 49, 1, "01");
    RequireField(wirehair, "WirehairProfile", 128, 32);
    RequireField(wirehair, "DescriptorCrc32c", 160, 4);
    REQUIRE(wirehair.crcGates.size() == 1);
    REQUIRE(wirehair.crcGates[0].Matches());

    const auto manifest = pbdump::DumpRecord(MakeManifestPayload(session), "final-manifest", context);
    REQUIRE(manifest.parseOk);
    RequireField(manifest, "DescriptorHeaderBytes", 2, 2, "0050");
    RequireField(manifest, "DescriptorTotalBytes", 4, 4, "00000054");
    RequireField(manifest, "SessionId", 8, 16, "000102030405060708090a0b0c0d0e0f");
    RequireField(manifest, "FileSize", 24, 8, "0000000000000075");
    RequireField(manifest, "SegmentCount", 32, 8, "0000000000000001");
    RequireField(manifest, "WholeFileDigest", 40, 32, "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
    RequireField(manifest, "DigestAlgorithm", 72, 1, "01");
    RequireField(manifest, "Reserved", 73, 7, "00000000000000");
    RequireField(manifest, "DescriptorCrc32c", 80, 4);
    REQUIRE(manifest.crcGates.size() == 1);
    REQUIRE(manifest.crcGates[0].Matches());
}

TEST_CASE("PBProtocolDump handles variable filename TLV and descriptor corruption without tail reinterpretation",
    "[tools][protocol-dump][descriptor-schema][bounds]")
{
    auto session = pbprotocol::test::MakeSessionDescriptor(117, 1);
    session.fileNameUtf8 = "x.bin";
    session.optionalExtensions = {std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0},
        std::byte{3}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    const auto bytes = SerializeSession(session);
    REQUIRE(bytes.size() == 90);
    const auto valid = pbdump::DumpRecord(bytes, "session-descriptor", {});
    REQUIRE(valid.parseOk);
    RequireField(valid, "FileNameUtf8", 70, 5, "782e62696e");
    RequireField(valid, "OptionalExtensions", 75, 11, "0100010003000000aabbcc");
    RequireField(valid, "DescriptorCrc32c", 86, 4);
    RequireSuccess(MakeSegmentPayload(session), "segment-descriptor", bytes);

    auto trailing = bytes;
    trailing.push_back(std::byte{0xEE});
    const auto appended = pbdump::DumpRecord(trailing, "session-descriptor", {});
    REQUIRE_FALSE(appended.parseOk);
    REQUIRE(appended.parseCode == pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    REQUIRE(appended.crcGates.size() == 1);
    REQUIRE(appended.crcGates[0].offset == 86);
    REQUIRE(appended.crcGates[0].Matches());
    RequireField(appended, "OptionalExtensions", 75, 11, "0100010003000000aabbcc");

    for (std::size_t length = 0; length < bytes.size(); length++)
    {
        CAPTURE(length);
        const auto truncated = pbdump::DumpRecord(std::span<const std::byte>(bytes).first(length), "session-descriptor", {});
        REQUIRE_FALSE(truncated.parseOk);
        REQUIRE(truncated.crcGates.empty());
        RequireNoField(truncated, "FileNameUtf8");
        RequireNoField(truncated, "OptionalExtensions");
        for (const auto& field : truncated.fields)
        {
            REQUIRE(field.offset <= length);
            REQUIRE(field.size <= length - field.offset);
        }
    }

    auto corrupt = bytes;
    corrupt.back() ^= std::byte{1};
    const auto badCrc = pbdump::DumpRecord(corrupt, "session-descriptor", {});
    REQUIRE(badCrc.parseCode == pbprotocol::ProtocolErrorCode::CrcMismatch);
    REQUIRE(badCrc.diagnosticOffset == 86);
    REQUIRE(badCrc.crcGates.size() == 1);
    REQUIRE_FALSE(badCrc.crcGates[0].Matches());
    REQUIRE(badCrc.diagnosticExpected != badCrc.diagnosticActual);

    for (const std::uint32_t declaredBytes : {4U, 0xFFFFFFFFU})
    {
        auto invalidSize = bytes;
        WriteUint32(invalidSize, 4, declaredBytes);
        const auto report = pbdump::DumpRecord(invalidSize, "session-descriptor", {});
        REQUIRE_FALSE(report.parseOk);
        REQUIRE(report.crcGates.empty());
        RequireNoField(report, "FileNameUtf8");
        RequireNoField(report, "OptionalExtensions");
    }

    auto invalidName = bytes;
    invalidName[70] = std::byte{'/'};
    RewriteDescriptorCrc(invalidName);
    const auto nameFailure = pbdump::DumpRecord(invalidName, "session-descriptor", {});
    REQUIRE(nameFailure.parseCode == pbprotocol::ProtocolErrorCode::InvalidFileName);
    REQUIRE(nameFailure.ParseCodeName() == "InvalidFileName");

    auto direct = MakeSegmentPayload(session);
    direct.resize(164, std::byte{0xEE});
    const auto directWithTail = pbdump::DumpRecord(direct, "segment-descriptor", bytes);
    REQUIRE_FALSE(directWithTail.parseOk);
    RequireNoField(directWithTail, "WirehairProfile");
    REQUIRE(directWithTail.crcGates.size() == 1);
    REQUIRE(directWithTail.crcGates[0].offset == 128);
    REQUIRE(directWithTail.crcGates[0].Matches());
}

TEST_CASE("PBProtocolDump rejects historical descriptors and names every appended protocol error",
    "[tools][protocol-dump][descriptor-schema][legacy]")
{
    struct LegacyCase
    {
        std::vector<std::byte> bytes;
        std::string type;
    };
    const std::array cases{
        LegacyCase{pbgolden::GenerateSessionDescriptorPayload(), "session-descriptor"},
        LegacyCase{pbgolden::GenerateDirectRepeatSegmentPayload(), "segment-descriptor"},
        LegacyCase{pbgolden::GenerateWirehairSegmentPayload(), "segment-descriptor"},
        LegacyCase{pbgolden::GenerateFinalManifestPayload(), "final-manifest"},
        LegacyCase{pbgolden::GenerateControlSessionDescriptor(), "control"}};
    const auto context = MakeSessionContext(200);
    for (const auto& legacy : cases)
    {
        CAPTURE(legacy.type, legacy.bytes.size());
        const auto report = pbdump::DumpRecord(legacy.bytes, legacy.type, context);
        REQUIRE_FALSE(report.parseOk);
        REQUIRE_FALSE(report.parseSkipped);
        REQUIRE(report.parseCode == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
        REQUIRE(report.ParseCodeName() == "UnsupportedDescriptorSchema");
        REQUIRE(report.diagnosticExpected == "valid-UnsupportedDescriptorSchema");
        if (legacy.type != "control")
        {
            RequireNoField(report, "ProtocolMajor");
            RequireNoField(report, "WirehairProfile");
            REQUIRE(report.crcGates.empty());
        }
    }

    const auto segment = MakeSegmentPayload(pbprotocol::test::MakeSessionDescriptor(117, 1));
    auto invalidContext = MakeSessionContext(117);
    invalidContext[0] = std::byte{2};
    const auto badContext = pbdump::DumpRecord(segment, "segment-descriptor", invalidContext);
    REQUIRE(badContext.parseSkipped);
    REQUIRE(badContext.contextStatus == "invalid-session-descriptor");
    REQUIRE(badContext.diagnosticExpected == "valid-UnsupportedDescriptorSchema");

    const std::array errors{
        std::pair{pbprotocol::ProtocolErrorCode::ResumeRecordConflict, "ResumeRecordConflict"},
        std::pair{pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, "ResumeStateIoFailure"},
        std::pair{pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete, "SegmentRecoveryIncomplete"},
        std::pair{pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema, "UnsupportedDescriptorSchema"},
        std::pair{pbprotocol::ProtocolErrorCode::InvalidFileName, "InvalidFileName"}};
    for (const auto& [code, name] : errors)
    {
        REQUIRE(pbdump::ProtocolErrorCodeName(code) == name);
    }
    REQUIRE(pbdump::ProtocolErrorCodeName(static_cast<pbprotocol::ProtocolErrorCode>(255)) == "Unknown(255)");
}
