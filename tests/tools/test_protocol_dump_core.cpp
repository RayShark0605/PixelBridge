#include "protocol_dump_core.h"

#include "pbgolden/golden_vector_source.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::byte> MakeSessionContext(const std::uint64_t fileSize)
{
    pbprotocol::SessionDescriptor descriptor;
    descriptor.protocolVersion.major = 1;
    descriptor.protocolVersion.minor = 0;
    for (std::size_t index = 0; index < descriptor.sessionId.bytes.size(); index++)
    {
        descriptor.sessionId.bytes[index] = std::byte{static_cast<std::uint8_t>(index)};
    }
    descriptor.originalFileSize = fileSize;
    descriptor.segmentCount = 1;
    descriptor.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
    std::vector<std::byte> bytes(pbprotocol::kSessionDescriptorPayloadBytes);
    REQUIRE(pbprotocol::SerializeSessionDescriptor(descriptor, bytes));
    return bytes;
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

TEST_CASE("PBProtocolDump accepts every forced Phase-0 record type", "[tools][protocol-dump]")
{
    const auto session117 = pbgolden::GenerateSessionDescriptorPayload();
    const auto session200 = MakeSessionContext(200);
    RequireSuccess(pbgolden::GenerateBootstrapRecord(), "bootstrap");
    RequireSuccess(pbgolden::GenerateControlSessionDescriptor(), "control");
    RequireSuccess(pbgolden::GenerateControlFragment(0), "fragment");
    RequireSuccess(session117, "session-descriptor");
    RequireSuccess(pbgolden::GenerateDirectRepeatSegmentPayload(),
        "segment-descriptor", session117);
    RequireSuccess(pbgolden::GenerateWirehairSegmentPayload(),
        "segment-descriptor", session200);
    RequireSuccess(pbgolden::GenerateFinalManifestPayload(), "final-manifest", session117);
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
        pbgolden::GenerateSessionDescriptorPayload(), "auto", {});
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
    const auto segment = pbgolden::GenerateDirectRepeatSegmentPayload();
    const auto missing = pbdump::DumpRecord(segment, "segment-descriptor", {});
    REQUIRE(missing.parseSkipped);
    REQUIRE(missing.contextStatus == "skipped");
    REQUIRE(missing.hasDiagnostic);
    REQUIRE(missing.diagnosticSpace == "context");
    REQUIRE(missing.diagnosticActual == "missing");

    auto invalidContext = MakeSessionContext(117);
    invalidContext[0] = std::byte{2};
    const auto invalid = pbdump::DumpRecord(
        segment, "segment-descriptor", invalidContext);
    REQUIRE(invalid.parseSkipped);
    REQUIRE(invalid.contextStatus == "invalid-session-descriptor");
    REQUIRE(invalid.diagnosticSpace == "context");
    REQUIRE(invalid.diagnosticOffset == 0);
    REQUIRE(invalid.diagnosticExpected == "valid-UnsupportedProtocolMajor");
    REQUIRE(invalid.diagnosticActual == "0x02");

    auto wrongContext = MakeSessionContext(117);
    wrongContext[4] ^= std::byte{1};
    const auto mismatch = pbdump::DumpRecord(segment, "segment-descriptor", wrongContext);
    REQUIRE_FALSE(mismatch.parseOk);
    REQUIRE_FALSE(mismatch.parseSkipped);
    REQUIRE(mismatch.parseCode == pbprotocol::ProtocolErrorCode::SessionTagMismatch);
}
