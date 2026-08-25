#include "protocol_dump_core.h"

#include "pbmodulation/modulation_result.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace pbdump {

namespace {

std::string HexDigits(const std::span<const std::byte> bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2u);
    for (const std::byte value : bytes)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        result.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
        result.push_back(kHexDigits[byteValue & 0xFu]);
    }
    return result;
}

// Every caller also guards the exact field range before using the value. The
// local checks keep these diagnostic helpers safe if a future field table
// accidentally omits that guard.

std::uint16_t ReadU16(const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.size() < 2 || offset > input.size() - 2)
    {
        return 0;
    }
    pbprotocol::ByteReader reader(input.subspan(offset, 2));
    const auto result = reader.ReadUint16();
    return result ? result.Value() : 0;
}

std::uint32_t ReadU32(const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.size() < 4 || offset > input.size() - 4)
    {
        return 0;
    }
    pbprotocol::ByteReader reader(input.subspan(offset, 4));
    const auto result = reader.ReadUint32();
    return result ? result.Value() : 0;
}

std::uint64_t ReadU64(const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.size() < 8 || offset > input.size() - 8)
    {
        return 0;
    }
    pbprotocol::ByteReader reader(input.subspan(offset, 8));
    const auto result = reader.ReadUint64();
    return result ? result.Value() : 0;
}

// Adds a field row only when the whole field is present in the input.
void AddField(std::vector<FieldRow>& fields, const std::size_t offset,
    const std::size_t size, const std::string& name,
    const std::string& value, const std::size_t inputSize)
{
    if (offset > inputSize || size > inputSize - offset)
    {
        return;
    }
    fields.push_back(FieldRow{offset, size, name, value});
}

// Typed field helpers. The bounds check happens before the bytes are read,
// so truncated inputs yield a shorter table instead of out-of-bounds
// access (the input span is untrusted in every dump scenario).

void AddU8Field(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::string& name)
{
    if (offset >= input.size())
    {
        return;
    }
    AddField(fields, offset, 1, name,
        HexValue(std::to_integer<std::uint8_t>(input[offset]), 2),
        input.size());
}

void AddU16Field(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::string& name)
{
    if (offset > input.size() || 2 > input.size() - offset)
    {
        return;
    }
    AddField(fields, offset, 2, name, HexValue(ReadU16(input, offset), 4),
        input.size());
}

void AddU32Field(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::string& name)
{
    if (offset > input.size() || 4 > input.size() - offset)
    {
        return;
    }
    AddField(fields, offset, 4, name, HexValue(ReadU32(input, offset), 8),
        input.size());
}

void AddU64Field(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::string& name)
{
    if (offset > input.size() || 8 > input.size() - offset)
    {
        return;
    }
    AddField(fields, offset, 8, name, HexValue(ReadU64(input, offset), 16),
        input.size());
}

void AddRawField(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::size_t size, const std::string& name)
{
    if (offset > input.size() || size > input.size() - offset)
    {
        return;
    }
    AddField(fields, offset, size, name,
        HexDigits(input.subspan(offset, size)), input.size());
}

void AddDigestField(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::string& name)
{
    AddRawField(fields, input, offset, 32, name);
}

// Payload preview: up to 16 bytes of hex, then "..." when truncated.
// `declaredSize` is the record-declared payload size (which may exceed the
// bytes actually present in a truncated input).
void AddPayloadPreview(std::vector<FieldRow>& fields,
    const std::span<const std::byte> input, const std::size_t offset,
    const std::size_t declaredSize, const std::string& name)
{
    if (offset > input.size())
    {
        return;
    }
    const std::size_t available = input.size() - offset;
    // The preview never exceeds the declared payload size: bytes beyond the
    // declared payload (a truncation tail or the following CRC field) are
    // not payload and must not be rendered as payload hex.
    const std::size_t payloadPreviewLimit =
        declaredSize < 16 ? declaredSize : 16;
    const std::size_t preview =
        available < payloadPreviewLimit ? available : payloadPreviewLimit;
    std::string value = HexDigits(input.subspan(offset, preview));
    if (declaredSize > preview)
    {
        value += "...";
    }
    fields.push_back(FieldRow{offset, declaredSize, name, value});
}

// Appends one CRC gate when the stored CRC bytes are present. The
// recomputed value covers input[coverageStart, coverageStart +
// coveredBytes): record-level gates cover from byte 0, while the
// Transport payload gate covers the payload region itself.
void AddCrcGate(DumpReport& report, const std::span<const std::byte> input,
    const std::string& crcFieldName, const std::size_t crcOffset,
    const std::size_t coverageStart, const std::size_t coveredBytes)
{
    if (crcOffset > input.size() || 4 > input.size() - crcOffset ||
        coverageStart > input.size() ||
        coveredBytes > input.size() - coverageStart)
    {
        return;
    }
    const std::uint32_t stored = ReadU32(input, crcOffset);
    const std::uint32_t recomputed = static_cast<std::uint32_t>(
        pbprotocol::ComputeCrc32c(
            input.subspan(coverageStart, coveredBytes)));
    report.crcGates.push_back(CrcGate{crcFieldName, crcOffset, stored,
        recomputed});
}

bool EqualsMagic(const std::span<const std::byte> input,
    const char* expected) noexcept
{
    if (input.size() < 4)
    {
        return false;
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        if (std::to_integer<std::uint8_t>(input[i]) !=
            static_cast<std::uint8_t>(expected[i]))
        {
            return false;
        }
    }
    return true;
}

std::string ModulationErrorCodeName(
    const pbmodulation::ModulationErrorCode code)
{
    switch (code)
    {
    case pbmodulation::ModulationErrorCode::None:
        return "Success";
    case pbmodulation::ModulationErrorCode::InvalidInput:
        return "InvalidInput";
    case pbmodulation::ModulationErrorCode::OutputBufferTooSmall:
        return "OutputBufferTooSmall";
    case pbmodulation::ModulationErrorCode::NonZeroReservedByte:
        return "NonZeroReservedByte";
    case pbmodulation::ModulationErrorCode::InvalidMagic:
        return "InvalidMagic";
    case pbmodulation::ModulationErrorCode::UnsupportedVersion:
        return "UnsupportedVersion";
    case pbmodulation::ModulationErrorCode::TruncatedInput:
        return "TruncatedInput";
    case pbmodulation::ModulationErrorCode::TrailingBytes:
        return "TrailingBytes";
    case pbmodulation::ModulationErrorCode::CrcMismatch:
        return "CrcMismatch";
    case pbmodulation::ModulationErrorCode::FrameGeometryMismatch:
        return "FrameGeometryMismatch";
    case pbmodulation::ModulationErrorCode::OffConstellationLevel:
        return "OffConstellationLevel";
    case pbmodulation::ModulationErrorCode::AmbiguousLevel:
        return "AmbiguousLevel";
    case pbmodulation::ModulationErrorCode::ChromaChannelMismatch:
        return "ChromaChannelMismatch";
    case pbmodulation::ModulationErrorCode::AlphaChannelViolation:
        return "AlphaChannelViolation";
    case pbmodulation::ModulationErrorCode::TornFrame:
        return "TornFrame";
    case pbmodulation::ModulationErrorCode::FrozenRegionMismatch:
        return "FrozenRegionMismatch";
    case pbmodulation::ModulationErrorCode::ManifestValidationFailed:
        return "ManifestValidationFailed";
    case pbmodulation::ModulationErrorCode::UnsupportedPngFormat:
        return "UnsupportedPngFormat";
    case pbmodulation::ModulationErrorCode::PngDecodeError:
        return "PngDecodeError";
    case pbmodulation::ModulationErrorCode::PngEncodeError:
        return "PngEncodeError";
    case pbmodulation::ModulationErrorCode::MemoryAllocationFailure:
        return "MemoryAllocationFailure";
    case pbmodulation::ModulationErrorCode::InternalInvariantViolation:
        return "InternalInvariantViolation";
    }
    return "Unknown(" +
        std::to_string(static_cast<int>(
            static_cast<std::underlying_type_t<
                pbmodulation::ModulationErrorCode>>(code))) + ")";
}

} // namespace


// ---------------------------------------------------------------------------
// Per-type field tables (every read is bounds-guarded).
// ---------------------------------------------------------------------------

namespace {

void AddBootstrapFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddRawField(fields, input, 0, 4, "Magic");
    AddU8Field(fields, input, 4, "Version");
    AddU8Field(fields, input, 5, "ProtocolMajor");
    AddU8Field(fields, input, 6, "ProtocolMinor");
    AddU8Field(fields, input, 7, "VisualLayoutVersion");
    AddU64Field(fields, input, 8, "VisualProfileId");
    AddU64Field(fields, input, 16, "SessionTag");
    AddU64Field(fields, input, 24, "FrameSequence");
    AddU32Field(fields, input, 32, "ControlEpoch");
    AddU32Field(fields, input, 36, "Flags");
    AddU32Field(fields, input, 40, "Crc32c");
    AddCrcGate(report, input, "Crc32c", 40, 0, 40);
}

void AddControlFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddRawField(fields, input, 0, 4, "Magic");
    AddU8Field(fields, input, 4, "Version");
    AddU8Field(fields, input, 5, "Type");
    AddU64Field(fields, input, 6, "ControlSequence");
    AddU64Field(fields, input, 14, "SessionTag");
    AddU32Field(fields, input, 22, "RecordBytes");
    // Locate payload and CRC from the checked declared boundary. Bytes
    // after RecordBytes are trailing input, never part of the payload.
    if (input.size() >= pbprotocol::kControlRecordPrefixBytes)
    {
        const std::size_t declaredRecordBytes = ReadU32(input, 22);
        if (declaredRecordBytes >= pbprotocol::kMinimumControlRecordBytes &&
            declaredRecordBytes <= input.size())
        {
            const std::size_t declaredPayloadBytes = declaredRecordBytes -
                pbprotocol::kMinimumControlRecordBytes;
            const std::size_t crcOffset = declaredRecordBytes - 4;
            AddPayloadPreview(fields, input, 26, declaredPayloadBytes, "Payload");
            AddU32Field(fields, input, crcOffset, "Crc32c");
            AddCrcGate(report, input, "Crc32c", crcOffset, 0, crcOffset);
        }
    }
}

void AddFragmentFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddU64Field(fields, input, 0, "RecordId");
    AddU16Field(fields, input, 8, "Index");
    AddU16Field(fields, input, 10, "Count");
    AddU32Field(fields, input, 12, "TotalRecordBytes");
    AddU16Field(fields, input, 16, "PayloadBytes");
    AddU16Field(fields, input, 18, "Flags");
    if (input.size() >= pbprotocol::kControlFragmentPrefixBytes)
    {
        const std::size_t declaredPayloadBytes = ReadU16(input, 16);
        const std::size_t declaredRecordBytes =
            pbprotocol::kControlFragmentPrefixBytes + declaredPayloadBytes;
        const std::size_t completeRecordBytes =
            declaredRecordBytes + pbprotocol::kControlFragmentCrcBytes;
        if (completeRecordBytes <= input.size())
        {
            AddPayloadPreview(fields, input,
                pbprotocol::kControlFragmentPrefixBytes,
                declaredPayloadBytes, "Payload");
            AddU32Field(fields, input, declaredRecordBytes, "Crc32c");
            AddCrcGate(report, input, "Crc32c", declaredRecordBytes, 0,
                declaredRecordBytes);
        }
    }
}

void AddSessionDescriptorFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddU16Field(fields, input, 0, "ProtocolMajor");
    AddU16Field(fields, input, 2, "ProtocolMinor");
    AddRawField(fields, input, 4, 16, "SessionId");
    AddU64Field(fields, input, 20, "FileSize");
    AddU64Field(fields, input, 28, "SegmentCount");
    AddU8Field(fields, input, 36, "DigestAlgorithm");
}

void AddSegmentDescriptorFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddU64Field(fields, input, 0, "SessionTag");
    AddU64Field(fields, input, 8, "SegmentOrdinal");
    AddU64Field(fields, input, 16, "RawOffset");
    AddU64Field(fields, input, 24, "RawSize");
    AddU64Field(fields, input, 32, "EncodedSize");
    AddU8Field(fields, input, 40, "CompressionCodec");
    AddU8Field(fields, input, 41, "OuterFecMode");
    AddU32Field(fields, input, 42, "OuterBlockBytes");
    AddDigestField(fields, input, 46, "RawDigest");
    AddDigestField(fields, input, 78, "EncodedDigest");
    if (input.size() >= 142)
    {
        AddDigestField(fields, input, 110, "WirehairProfile");
    }
}

void AddFinalManifestFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddRawField(fields, input, 0, 16, "SessionId");
    AddU64Field(fields, input, 16, "FileSize");
    AddU64Field(fields, input, 24, "SegmentCount");
    AddDigestField(fields, input, 32, "WholeFileDigest");
    AddU8Field(fields, input, 64, "DigestAlgorithm");
}

void AddTransportFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddU8Field(fields, input, 0, "BlockType");
    AddU8Field(fields, input, 1, "ProtocolMinor");
    AddU16Field(fields, input, 2, "Flags");
    AddU64Field(fields, input, 4, "SessionTag");
    AddU64Field(fields, input, 12, "SegmentOrdinal");
    AddU32Field(fields, input, 20, "OuterBlockId");
    AddU16Field(fields, input, 24, "PayloadBytes");
    AddU16Field(fields, input, 26, "Reserved");
    AddU32Field(fields, input, 28, "HeaderCrc32c");
    if (input.size() >= pbprotocol::kTransportHeaderBytes)
    {
        const std::size_t declaredPayloadBytes =
            static_cast<std::size_t>(ReadU16(input, 24));
        AddCrcGate(report, input, "HeaderCrc32c", 28, 0,
            pbprotocol::kTransportHeaderCrcCoverageBytes);
        const std::size_t payloadCrcOffset =
            pbprotocol::kTransportPayloadOffset + declaredPayloadBytes;
        const std::size_t declaredBlockBytes =
            payloadCrcOffset + pbprotocol::kTransportPayloadCrcBytes;
        if (declaredBlockBytes <= input.size())
        {
            AddPayloadPreview(fields, input,
                pbprotocol::kTransportPayloadOffset,
                declaredPayloadBytes, "Payload");
            AddU32Field(fields, input, payloadCrcOffset, "PayloadCrc32c");
            AddCrcGate(report, input, "PayloadCrc32c", payloadCrcOffset,
                pbprotocol::kTransportPayloadOffset, declaredPayloadBytes);
        }
    }
}

void AddWirehairDescriptorFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddRawField(fields, input, 0, 4, "Magic");
    AddU8Field(fields, input, 4, "Version");
    AddU8Field(fields, input, 5, "Reserved");
    AddU16Field(fields, input, 6, "LayoutTag");
    AddU64Field(fields, input, 8, "ProfileId");
    AddU64Field(fields, input, 16, "MessageBytes");
    AddU32Field(fields, input, 24, "BlockBytes");
    AddU8Field(fields, input, 28, "SeedAttempt");
    AddRawField(fields, input, 29, 3, "Reserved");
}

void AddPbvmManifestFields(DumpReport& report,
    const std::span<const std::byte> input)
{
    auto& fields = report.fields;
    AddRawField(fields, input, 0, 4, "Magic");
    AddU8Field(fields, input, 4, "Version");
    AddU8Field(fields, input, 5, "LayoutVersion");
    AddU16Field(fields, input, 6, "Reserved");
    AddU32Field(fields, input, 8, "CanvasWidth");
    AddU32Field(fields, input, 12, "CanvasHeight");
    AddU8Field(fields, input, 16, "RegionCount");
    AddU8Field(fields, input, 17, "BitsPerSymbol");
    AddU8Field(fields, input, 18, "LevelCount");
    AddU8Field(fields, input, 19, "LevelBase");
    AddU8Field(fields, input, 20, "LevelStep");
    AddU8Field(fields, input, 21, "DataTileWidth");
    AddU8Field(fields, input, 22, "DataTileHeight");
    AddU8Field(fields, input, 23, "LaneSymbolWidth");
    AddU8Field(fields, input, 24, "LaneSymbolHeight");
    AddRawField(fields, input, 25, 8, "Reserved");
    for (std::size_t regionIndex = 0; regionIndex < 14; regionIndex++)
    {
        const std::size_t regionOffset = 33 + regionIndex * 17;
        const std::string prefix =
            "Region" + std::to_string(regionIndex);
        AddU8Field(fields, input, regionOffset, prefix + ".Type");
        AddU32Field(fields, input, regionOffset + 1, prefix + ".X");
        AddU32Field(fields, input, regionOffset + 5, prefix + ".Y");
        AddU32Field(fields, input, regionOffset + 9, prefix + ".Width");
        AddU32Field(fields, input, regionOffset + 13, prefix + ".Height");
    }
    if (input.size() >= 275)
    {
        AddU32Field(fields, input, 271, "Crc32c");
        AddCrcGate(report, input, "Crc32c", 271, 0, 271);
    }
}

std::string ByteValueAt(const std::span<const std::byte> input,
    const std::size_t offset)
{
    if (offset >= input.size())
    {
        return "<eof>";
    }
    return "0x" + HexValue(std::to_integer<std::uint8_t>(input[offset]), 2);
}

void BuildFailureDiagnostic(DumpReport& report,
    const std::span<const std::byte> input,
    const bool contextWasProvided)
{
    if (report.parseOk)
    {
        return;
    }
    if (report.hasDiagnostic)
    {
        return;
    }
    if (report.parseSkipped)
    {
        report.hasDiagnostic = true;
        report.diagnosticSpace = "context";
        report.diagnosticOffset = 0;
        report.diagnosticExpected = "37-byte-session-descriptor";
        report.diagnosticActual = contextWasProvided ? "invalid" : "missing";
        return;
    }
    const bool parserReportedCrcMismatch =
        report.parseCode == pbprotocol::ProtocolErrorCode::CrcMismatch ||
        report.parseDetail == "modulation:CrcMismatch";
    for (const CrcGate& gate : report.crcGates)
    {
        if (parserReportedCrcMismatch && !gate.Matches() &&
            gate.offset == report.parseOffset)
        {
            report.hasDiagnostic = true;
            report.diagnosticOffset = gate.offset;
            report.diagnosticExpected = "0x" + HexValue(gate.recomputed, 8);
            report.diagnosticActual = "0x" + HexValue(gate.stored, 8);
            return;
        }
    }
    if (report.type == "control" && input.size() >= pbprotocol::kControlRecordPrefixBytes)
    {
        const std::size_t declaredSize = ReadU32(input, 22);
        if (declaredSize != input.size())
        {
            report.hasDiagnostic = true;
            report.diagnosticOffset = std::min(declaredSize, input.size());
            report.diagnosticExpected = declaredSize < input.size() ? "<eof>" : "byte";
            report.diagnosticActual = declaredSize < input.size()
                ? ByteValueAt(input, declaredSize) : "<eof>";
            return;
        }
    }
    if (report.type == "fragment" && input.size() >= pbprotocol::kControlFragmentPrefixBytes)
    {
        const std::size_t declaredSize = pbprotocol::kControlFragmentPrefixBytes +
            static_cast<std::size_t>(ReadU16(input, 16)) + pbprotocol::kControlFragmentCrcBytes;
        if (declaredSize != input.size())
        {
            report.hasDiagnostic = true;
            report.diagnosticOffset = std::min(declaredSize, input.size());
            report.diagnosticExpected = declaredSize < input.size() ? "<eof>" : "byte";
            report.diagnosticActual = declaredSize < input.size()
                ? ByteValueAt(input, declaredSize) : "<eof>";
            return;
        }
    }
    report.hasDiagnostic = true;
    report.diagnosticOffset = report.parseOffset;
    switch (report.parseCode)
    {
    case pbprotocol::ProtocolErrorCode::TruncatedInput:
        report.diagnosticExpected = "byte";
        report.diagnosticActual = "<eof>";
        break;
    case pbprotocol::ProtocolErrorCode::TrailingBytes:
        report.diagnosticExpected = "<eof>";
        report.diagnosticActual = ByteValueAt(input, report.parseOffset);
        break;
    case pbprotocol::ProtocolErrorCode::NonZeroReservedByte:
    case pbprotocol::ProtocolErrorCode::NonZeroReservedBits:
        report.diagnosticExpected = "0x00";
        report.diagnosticActual = ByteValueAt(input, report.parseOffset);
        break;
    default:
        report.diagnosticExpected = "valid-" + ProtocolErrorCodeName(report.parseCode);
        report.diagnosticActual = ByteValueAt(input, report.parseOffset);
        break;
    }
}

} // namespace


// ---------------------------------------------------------------------------
// Authoritative parse per type.
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kControlPayloadOffset =
    pbprotocol::kControlRecordPrefixBytes; // 26

// Parses the optional session-descriptor context once per record.
// Returns true when the context is a valid 37-byte session descriptor.
bool TryParseSessionContext(
    const std::span<const std::byte> sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    DumpReport& report,
    pbprotocol::SessionDescriptor& outSession)
{
    if (sessionDescriptor.empty())
    {
        return false;
    }
    const auto result = pbprotocol::ParseSessionDescriptor(
        sessionDescriptor, resourcePolicy);
    if (!result)
    {
        report.contextStatus = "invalid-session-descriptor";
        report.hasDiagnostic = true;
        report.diagnosticSpace = "context";
        report.diagnosticOffset = result.Error().offset;
        report.diagnosticExpected = "valid-" +
            ProtocolErrorCodeName(result.Error().code);
        report.diagnosticActual =
            ByteValueAt(sessionDescriptor, result.Error().offset);
        return false;
    }
    outSession = result.Value();
    return true;
}

void ParseBootstrap(DumpReport& report,
    const std::span<const std::byte> input)
{
    const auto result = pbprotocol::ParseBootstrapRecord(input);
    if (result)
    {
        report.parseOk = true;
        return;
    }
    report.parseCode = result.Error().code;
    report.parseOffset = result.Error().offset;
}

void ParseControl(DumpReport& report,
    const std::span<const std::byte> input,
    const std::span<const std::byte> sessionDescriptor)
{
    const auto result = pbprotocol::ParseControlRecord(input);
    if (!result)
    {
        report.parseCode = result.Error().code;
        report.parseOffset = result.Error().offset;
        return;
    }
    const auto& record = result.Value();
    const std::span<const std::byte> payload = record.payload;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    switch (record.recordType)
    {
    case pbprotocol::ControlRecordType::SessionDescriptor:
    {
        const auto payloadResult =
            pbprotocol::ParseSessionDescriptor(payload, resourcePolicy);
        if (!payloadResult)
        {
            report.parseCode = payloadResult.Error().code;
            report.parseOffset =
                kControlPayloadOffset + payloadResult.Error().offset;
        }
        else
        {
            report.parseOk = true;
        }
        break;
    }
    case pbprotocol::ControlRecordType::SegmentDescriptor:
    case pbprotocol::ControlRecordType::FinalManifest:
    {
        pbprotocol::SessionDescriptor session{};
        if (!TryParseSessionContext(
            sessionDescriptor, resourcePolicy, report, session))
        {
            // No usable context: the semantic cross-check cannot run.
            if (report.contextStatus != "invalid-session-descriptor")
            {
                report.contextStatus = "skipped";
            }
            report.parseSkipped = true;
            return;
        }
        if (record.recordType ==
            pbprotocol::ControlRecordType::SegmentDescriptor)
        {
            const auto segmentResult =
                pbprotocol::ParseSegmentDescriptor(
                    payload, session, resourcePolicy);
            if (!segmentResult)
            {
                report.parseCode = segmentResult.Error().code;
                report.parseOffset =
                    kControlPayloadOffset + segmentResult.Error().offset;
                report.contextStatus =
                    segmentResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::SessionTagMismatch
                        ? "SessionTagMismatch"
                        : "checked";
                return;
            }
        }
        else
        {
            const auto manifestResult =
                pbprotocol::ParseFinalManifest(
                    payload, session, resourcePolicy);
            if (!manifestResult)
            {
                report.parseCode = manifestResult.Error().code;
                report.parseOffset =
                    kControlPayloadOffset +
                    manifestResult.Error().offset;
                report.contextStatus = "checked";
                return;
            }
        }
        report.parseOk = true;
        report.contextStatus = "checked";
        break;
    }
    default:
        // ParseControlRecord rejects unknown types before this point.
        break;
    }
}

void ParseFragment(DumpReport& report,
    const std::span<const std::byte> input)
{
    const auto result = pbprotocol::ParseControlFragment(input);
    if (result)
    {
        report.parseOk = true;
        return;
    }
    report.parseCode = result.Error().code;
    report.parseOffset = result.Error().offset;
}

void ParseSessionDescriptorPayload(DumpReport& report,
    const std::span<const std::byte> input)
{
    const auto result = pbprotocol::ParseSessionDescriptor(
        input, pbprotocol::GetDefaultReceiverResourcePolicy());
    if (result)
    {
        report.parseOk = true;
        return;
    }
    report.parseCode = result.Error().code;
    report.parseOffset = result.Error().offset;
}

void ParseSegmentDescriptorPayload(DumpReport& report,
    const std::span<const std::byte> input,
    const std::span<const std::byte> sessionDescriptor)
{
    pbprotocol::SessionDescriptor session{};
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    if (!TryParseSessionContext(
        sessionDescriptor, resourcePolicy, report, session))
    {
        if (report.contextStatus != "invalid-session-descriptor")
        {
            report.contextStatus = "skipped";
        }
        report.parseSkipped = true;
        return;
    }
    const auto result = pbprotocol::ParseSegmentDescriptor(
        input, session, resourcePolicy);
    if (!result)
    {
        report.parseCode = result.Error().code;
        report.parseOffset = result.Error().offset;
        report.contextStatus =
            result.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagMismatch
                ? "SessionTagMismatch"
                : "checked";
        return;
    }
    report.parseOk = true;
    report.contextStatus = "checked";
}

void ParseFinalManifestPayload(DumpReport& report,
    const std::span<const std::byte> input,
    const std::span<const std::byte> sessionDescriptor)
{
    pbprotocol::SessionDescriptor session{};
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    if (!TryParseSessionContext(
        sessionDescriptor, resourcePolicy, report, session))
    {
        if (report.contextStatus != "invalid-session-descriptor")
        {
            report.contextStatus = "skipped";
        }
        report.parseSkipped = true;
        return;
    }
    const auto result = pbprotocol::ParseFinalManifest(
        input, session, resourcePolicy);
    if (!result)
    {
        report.parseCode = result.Error().code;
        report.parseOffset = result.Error().offset;
        report.contextStatus = "checked";
        return;
    }
    report.parseOk = true;
    report.contextStatus = "checked";
}

void ParseTransport(DumpReport& report,
    const std::span<const std::byte> input)
{
    const auto result = pbprotocol::ParseTransportBlock(input);
    if (result)
    {
        report.parseOk = true;
        return;
    }
    report.parseCode = result.Error().code;
    report.parseOffset = result.Error().offset;
}

void ParseWirehairDescriptor(DumpReport& report,
    const std::span<const std::byte> input)
{
    constexpr std::size_t profileBytes =
        pbprotocol::kWirehairV2SerializedProfileBytes;
    if (input.size() < profileBytes)
    {
        report.parseCode = pbprotocol::ProtocolErrorCode::TruncatedInput;
        report.parseOffset = input.size();
        return;
    }
    if (input.size() > profileBytes)
    {
        report.parseCode = pbprotocol::ProtocolErrorCode::TrailingBytes;
        report.parseOffset = profileBytes;
        return;
    }
    // Standalone descriptor probe: the self-declared MessageBytes /
    // BlockBytes fields are the expected dimensions, so the structural
    // gates (magic, version, layout tag, certified profile id, K range,
    // reserved bytes) all still apply.
    const std::uint64_t messageBytes = ReadU64(input, 16);
    const std::uint32_t blockBytes = static_cast<std::uint32_t>(
        ReadU32(input, 24));
    pbprotocol::WirehairV2SerializedProfile profile{};
    for (std::size_t i = 0; i < profileBytes; i++)
    {
        profile.bytes[i] = input[i];
    }
    const auto status = pbprotocol::ValidateWirehairV2SerializedProfile(
        profile, messageBytes, blockBytes, 0);
    if (status)
    {
        report.parseOk = true;
        return;
    }
    report.parseCode = status.Error().code;
    report.parseOffset = status.Error().offset;
}

void ParsePbvmManifest(DumpReport& report,
    const std::span<const std::byte> input)
{
    const auto result =
        pbmodulation::ParseReferenceRegionManifest(input);
    if (result)
    {
        report.parseOk = true;
        return;
    }
    // The modulation namespace is mapped onto the uniform protocol
    // namespace for the status line; the modulation-side code name is
    // preserved in the detail field so no diagnostic information is lost.
    report.parseCode = pbprotocol::ProtocolErrorCode::InvalidRecordSize;
    report.parseOffset = result.Error().offset;
    report.parseDetail = "modulation:" +
        ModulationErrorCodeName(result.Error().code);
}

} // namespace

std::string DumpReport::ParseCodeName() const
{
    if (parseOk)
    {
        return "Success";
    }
    if (parseSkipped)
    {
        return "skipped";
    }
    return ProtocolErrorCodeName(parseCode);
}


std::string ProtocolErrorCodeName(
    const pbprotocol::ProtocolErrorCode code)
{
    switch (code)
    {
    case pbprotocol::ProtocolErrorCode::None:
        return "Success";
    case pbprotocol::ProtocolErrorCode::TruncatedInput:
        return "TruncatedInput";
    case pbprotocol::ProtocolErrorCode::OutputBufferTooSmall:
        return "OutputBufferTooSmall";
    case pbprotocol::ProtocolErrorCode::LengthNarrowing:
        return "LengthNarrowing";
    case pbprotocol::ProtocolErrorCode::LengthOverflow:
        return "LengthOverflow";
    case pbprotocol::ProtocolErrorCode::LengthLimitExceeded:
        return "LengthLimitExceeded";
    case pbprotocol::ProtocolErrorCode::InvalidUtf8:
        return "InvalidUtf8";
    case pbprotocol::ProtocolErrorCode::NonZeroReservedByte:
        return "NonZeroReservedByte";
    case pbprotocol::ProtocolErrorCode::NonCanonicalPadding:
        return "NonCanonicalPadding";
    case pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor:
        return "UnsupportedProtocolMajor";
    case pbprotocol::ProtocolErrorCode::UnknownMandatoryFeature:
        return "UnknownMandatoryFeature";
    case pbprotocol::ProtocolErrorCode::ConflictingFeatureFlags:
        return "ConflictingFeatureFlags";
    case pbprotocol::ProtocolErrorCode::TrailingBytes:
        return "TrailingBytes";
    case pbprotocol::ProtocolErrorCode::InvalidLengthPrefixWidth:
        return "InvalidLengthPrefixWidth";
    case pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor:
        return "UnsupportedProtocolMinor";
    case pbprotocol::ProtocolErrorCode::InvalidEnumValue:
        return "InvalidEnumValue";
    case pbprotocol::ProtocolErrorCode::InvalidRecordSize:
        return "InvalidRecordSize";
    case pbprotocol::ProtocolErrorCode::InvalidDescriptor:
        return "InvalidDescriptor";
    case pbprotocol::ProtocolErrorCode::InvalidResourcePolicy:
        return "InvalidResourcePolicy";
    case pbprotocol::ProtocolErrorCode::ResourceLimitExceeded:
        return "ResourceLimitExceeded";
    case pbprotocol::ProtocolErrorCode::ResourceExhausted:
        return "ResourceExhausted";
    case pbprotocol::ProtocolErrorCode::SessionTagMismatch:
        return "SessionTagMismatch";
    case pbprotocol::ProtocolErrorCode::SessionMismatch:
        return "SessionMismatch";
    case pbprotocol::ProtocolErrorCode::UnknownSession:
        return "UnknownSession";
    case pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange:
        return "SegmentOrdinalOutOfRange";
    case pbprotocol::ProtocolErrorCode::SegmentRangeOutOfBounds:
        return "SegmentRangeOutOfBounds";
    case pbprotocol::ProtocolErrorCode::SegmentOverlap:
        return "SegmentOverlap";
    case pbprotocol::ProtocolErrorCode::SegmentGap:
        return "SegmentGap";
    case pbprotocol::ProtocolErrorCode::SegmentMapIncomplete:
        return "SegmentMapIncomplete";
    case pbprotocol::ProtocolErrorCode::DescriptorConflict:
        return "DescriptorConflict";
    case pbprotocol::ProtocolErrorCode::InvalidWirehairProfile:
        return "InvalidWirehairProfile";
    case pbprotocol::ProtocolErrorCode::MissingFinalManifest:
        return "MissingFinalManifest";
    case pbprotocol::ProtocolErrorCode::DigestMismatch:
        return "DigestMismatch";
    case pbprotocol::ProtocolErrorCode::InternalDescriptorStateError:
        return "InternalDescriptorStateError";
    case pbprotocol::ProtocolErrorCode::InternalInvariantViolation:
        return "InternalInvariantViolation";
    case pbprotocol::ProtocolErrorCode::SessionTagCollision:
        return "SessionTagCollision";
    case pbprotocol::ProtocolErrorCode::CsprngFailure:
        return "CsprngFailure";
    case pbprotocol::ProtocolErrorCode::InvalidMagic:
        return "InvalidMagic";
    case pbprotocol::ProtocolErrorCode::UnsupportedBootstrapVersion:
        return "UnsupportedBootstrapVersion";
    case pbprotocol::ProtocolErrorCode::UnsupportedControlVersion:
        return "UnsupportedControlVersion";
    case pbprotocol::ProtocolErrorCode::CrcMismatch:
        return "CrcMismatch";
    case pbprotocol::ProtocolErrorCode::NonZeroReservedBits:
        return "NonZeroReservedBits";
    case pbprotocol::ProtocolErrorCode::InvalidControlFragment:
        return "InvalidControlFragment";
    case pbprotocol::ProtocolErrorCode::ControlFragmentConflict:
        return "ControlFragmentConflict";
    case pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded:
        return "ControlReassemblyQuotaExceeded";
    case pbprotocol::ProtocolErrorCode::InvalidObservationOrdinal:
        return "InvalidObservationOrdinal";
    case pbprotocol::ProtocolErrorCode::OutputReservationDenied:
        return "OutputReservationDenied";
    case pbprotocol::ProtocolErrorCode::OrphanPayloadConflict:
        return "OrphanPayloadConflict";
    case pbprotocol::ProtocolErrorCode::UnknownSegment:
        return "UnknownSegment";
    case pbprotocol::ProtocolErrorCode::OverlappingSpans:
        return "OverlappingSpans";
    }
    return "Unknown(" +
        std::to_string(static_cast<int>(
            static_cast<std::underlying_type_t<
                pbprotocol::ProtocolErrorCode>>(code))) + ")";
}

std::string HexValue(const std::uint64_t value,
    const std::size_t hexDigits)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hexDigits);
    for (std::size_t i = hexDigits; i > 0; i--)
    {
        const std::size_t nibbleIndex = i - 1;
        const std::uint64_t nibble = nibbleIndex < 16
            ? (value >> (nibbleIndex * 4)) & 0xFu : 0;
        result.push_back(kHexDigits[nibble]);
    }
    return result;
}

DumpReport DumpRecord(const std::span<const std::byte> input,
    const std::string& typeHint,
    const std::span<const std::byte> sessionDescriptor)
{
    DumpReport report;
    report.totalBytes = input.size();

    std::string detectedType = "unrecognized";
    if (typeHint == "auto" || typeHint.empty())
    {
        if (input.size() >= 4)
        {
            if (EqualsMagic(input, "PBRG"))
            {
                detectedType = "bootstrap";
            }
            else if (EqualsMagic(input, "PBCR"))
            {
                detectedType = "control";
            }
            else if (EqualsMagic(input, "PBVM"))
            {
                detectedType = "pbvm-manifest";
            }
            else if (EqualsMagic(input, "WHV2"))
            {
                detectedType = "wirehair-descriptor";
            }
        }
        if (detectedType == "unrecognized")
        {
            // Magic-less probe: a self-consistent Transport block passes
            // its header CRC and exact-length gate.
            if (input.size() >=
                pbprotocol::kTransportMinimumBlockBytes)
            {
                if (pbprotocol::ParseTransportBlock(input))
                {
                    detectedType = "transport";
                }
            }
        }
    }
    else
    {
        detectedType = typeHint;
    }

    if (detectedType == "bootstrap")
    {
        report.type = detectedType;
        AddBootstrapFields(report, input);
        ParseBootstrap(report, input);
    }
    else if (detectedType == "control")
    {
        report.type = detectedType;
        AddControlFields(report, input);
        ParseControl(report, input, sessionDescriptor);
    }
    else if (detectedType == "fragment")
    {
        report.type = detectedType;
        AddFragmentFields(report, input);
        ParseFragment(report, input);
    }
    else if (detectedType == "session-descriptor")
    {
        report.type = detectedType;
        AddSessionDescriptorFields(report, input);
        ParseSessionDescriptorPayload(report, input);
    }
    else if (detectedType == "segment-descriptor")
    {
        report.type = detectedType;
        AddSegmentDescriptorFields(report, input);
        ParseSegmentDescriptorPayload(report, input, sessionDescriptor);
    }
    else if (detectedType == "final-manifest")
    {
        report.type = detectedType;
        AddFinalManifestFields(report, input);
        ParseFinalManifestPayload(report, input, sessionDescriptor);
    }
    else if (detectedType == "transport")
    {
        report.type = detectedType;
        AddTransportFields(report, input);
        ParseTransport(report, input);
    }
    else if (detectedType == "wirehair-descriptor")
    {
        report.type = detectedType;
        AddWirehairDescriptorFields(report, input);
        ParseWirehairDescriptor(report, input);
    }
    else if (detectedType == "pbvm-manifest")
    {
        report.type = detectedType;
        AddPbvmManifestFields(report, input);
        ParsePbvmManifest(report, input);
    }
    else
    {
        report.type = "unrecognized";
    }
    if (report.type != "unrecognized")
    {
        BuildFailureDiagnostic(report, input, !sessionDescriptor.empty());
    }
    return report;
}

std::string FormatDump(const DumpReport& report)
{
    std::string out;
    if (report.type == "unrecognized")
    {
        out += "[detect] unrecognized\n";
        out += "[size] " + std::to_string(report.totalBytes) + "\n";
        return out;
    }
    out += "[type] " + report.type + "\n";
    out += "[size] " + std::to_string(report.totalBytes) + "\n";
    for (const auto& field : report.fields)
    {
        out += "[field] offset=" + std::to_string(field.offset) +
            " size=" + std::to_string(field.size) +
            " name=" + field.name +
            " value=" + field.value + "\n";
    }
    for (const auto& gate : report.crcGates)
    {
        out += "[crc] field=" + gate.fieldName +
            " offset=" + std::to_string(gate.offset) +
            " stored=0x" + HexValue(gate.stored, 8) +
            " recomputed=0x" + HexValue(gate.recomputed, 8) +
            " status=" + (gate.Matches() ? "match" : "mismatch") + "\n";
    }
    out += "[parse] status=" + report.ParseCodeName() +
        " offset=" + std::to_string(report.parseOffset);
    if (!report.parseDetail.empty())
    {
        out += " detail=" + report.parseDetail;
    }
    out += "\n";
    if (report.contextStatus != "none")
    {
        out += "[context] status=" + report.contextStatus + "\n";
    }
    if (report.hasDiagnostic)
    {
        out += "[diagnostic] space=" + report.diagnosticSpace +
            " byte_offset=" + std::to_string(report.diagnosticOffset) +
            " expected=" + report.diagnosticExpected +
            " actual=" + report.diagnosticActual + "\n";
    }
    return out;
}

} // namespace pbdump
