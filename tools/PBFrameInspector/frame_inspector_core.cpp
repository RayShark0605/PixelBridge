#include "frame_inspector_core.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace pbinspect {

namespace {

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

std::string ProtocolErrorCodeName(
    const pbprotocol::ProtocolErrorCode code)
{
    // The inspector surfaces protocol parse codes only in diagnostics
    // (bootstrap / control / transport). Keep a complete local name table so
    // the tool remains independent of PBProtocolDump without degrading a
    // newly reached protocol error to an opaque integer.
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

std::string ToHexLower(
    const std::span<const std::byte> data)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2u);
    for (const std::byte value : data)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        result.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
        result.push_back(kHexDigits[byteValue & 0xFu]);
    }
    return result;
}

std::string ToHexLower(const std::uint64_t value)
{
    // Value rendering: most significant byte first (the integer's hex
    // notation), not the little-endian wire byte order.
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < 8; i++)
    {
        bytes[i] =
            std::byte{static_cast<std::uint8_t>(
                value >> static_cast<unsigned int>((7U - i) * 8U))};
    }
    return ToHexLower(std::span<const std::byte>(bytes));
}

// Big-endian u32 (PNG container fields) / little-endian readers.

std::uint32_t ReadU32BigEndian(
    const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset])) << 24) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + 2])) << 8) |
        static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + 3])));
}

std::uint32_t ReadU32LittleEndian(
    const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; i++)
    {
        // Little-endian: the first byte read is the least significant.
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + i])) <<
            static_cast<unsigned int>(i * 8U);
    }
    return value;
}

std::uint64_t ReadU64LittleEndian(
    const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; i++)
    {
        // Little-endian: the first byte read is the least significant.
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(input[offset + i])) <<
            static_cast<unsigned int>(i * 8U);
    }
    return value;
}

bool IsAllZero(const std::span<const std::byte> data) noexcept
{
    for (const std::byte value : data)
    {
        if (value != std::byte{0})
        {
            return false;
        }
    }
    return true;
}

bool BytesEqual(const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); i++)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

std::string ByteHex(const std::byte value)
{
    const std::array<std::byte, 1> bytes{value};
    return "0x" + ToHexLower(bytes);
}

void AddDiagnostic(InspectorReport& report, const std::string& space,
    const std::size_t offset, const std::string& expected,
    const std::string& actual)
{
    FailureDiagnostic diagnostic;
    diagnostic.space = space;
    diagnostic.byteOffset = offset;
    diagnostic.expected = expected;
    diagnostic.actual = actual;
    report.diagnostics.push_back(std::move(diagnostic));
}

void AddRecoveryDiagnostic(InspectorReport& report,
    const std::size_t windowByteOffset, const std::size_t innerByteOffset,
    const std::string& expected, const std::string& actual)
{
    FailureDiagnostic diagnostic;
    diagnostic.space = "data-region";
    diagnostic.byteOffset = windowByteOffset + innerByteOffset;
    diagnostic.expected = expected;
    diagnostic.actual = actual;
    diagnostic.windowByteOffset = windowByteOffset;
    diagnostic.innerByteOffset = innerByteOffset;
    diagnostic.dataRegionByteOffset = windowByteOffset + innerByteOffset;
    diagnostic.hasNestedOffsets = true;
    report.diagnostics.push_back(std::move(diagnostic));
}

void SetContainerError(InspectorReport& report, const std::string& error,
    const std::size_t offset, const std::string& expected,
    const std::string& actual)
{
    report.containerError = error;
    report.containerErrorOffset = offset;
    AddDiagnostic(report, "container", offset, expected, actual);
}

} // namespace


namespace {

constexpr std::array<std::byte, 8> kPngSignature{
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4E},
    std::byte{0x47}, std::byte{0x0D}, std::byte{0x0A},
    std::byte{0x1A}, std::byte{0x0A}};

std::string ReferenceRegionTypeName(
    const pbmodulation::ReferenceRegionType type)
{
    switch (type)
    {
    case pbmodulation::ReferenceRegionType::Guard:
        return "Guard";
    case pbmodulation::ReferenceRegionType::Sync:
        return "Sync";
    case pbmodulation::ReferenceRegionType::BootstrapA:
        return "BootstrapA";
    case pbmodulation::ReferenceRegionType::Control:
        return "Control";
    case pbmodulation::ReferenceRegionType::Pilot:
        return "Pilot";
    case pbmodulation::ReferenceRegionType::DataGrid:
        return "DataGrid";
    case pbmodulation::ReferenceRegionType::BootstrapB:
        return "BootstrapB";
    }
    return "Unknown";
}

void FillRegionTable(InspectorReport& report)
{
    for (std::size_t i = 0; i < pbmodulation::kReferenceRegions.size(); i++)
    {
        const auto& region = pbmodulation::kReferenceRegions[i];
        RegionRow row;
        row.index = i;
        row.typeName = ReferenceRegionTypeName(region.type);
        row.x = region.x;
        row.y = region.y;
        row.width = region.width;
        row.height = region.height;
        row.pixelOffset =
            static_cast<std::size_t>(region.y) *
                pbmodulation::kReferenceCanvasWidth +
            region.x;
        row.pixelBytes =
            static_cast<std::size_t>(region.width) * region.height * 4u;
        report.regions.push_back(row);
    }
}

// PBRW container gate: every field is validated BEFORE any canvas
// allocation; a hostile geometry (anything but the frozen 1920x1080)
// stops the inspection at the geometry check with zero allocation.
// Returns true when the input passed the gate and the canvas was
// decoded into outCanvas.
bool InspectPbrwContainer(
    const std::span<const std::byte> input,
    InspectorReport& report,
    std::vector<std::byte>& outCanvas)
{
    report.containerType = "pbrw";
    constexpr std::size_t kHeaderBytes =
        pbmodulation::kRawFrameHeaderBytes; // 28
    if (input.size() < kHeaderBytes)
    {
        SetContainerError(report, "TruncatedInput", input.size(), "byte", "<eof>");
        return false;
    }
    const std::uint8_t version =
        std::to_integer<std::uint8_t>(input[4]);
    if (version != pbmodulation::kRawFrameVersion)
    {
        SetContainerError(report, "UnsupportedVersion", 4,
            ByteHex(std::byte{pbmodulation::kRawFrameVersion}), ByteHex(input[4]));
        return false;
    }
    for (std::size_t i = 5; i < 8; i++)
    {
        if (input[i] != std::byte{0})
        {
            SetContainerError(report, "NonZeroReservedByte", i, "0x00", ByteHex(input[i]));
            return false;
        }
    }
    const std::uint32_t width = ReadU32LittleEndian(input, 8);
    const std::uint32_t height = ReadU32LittleEndian(input, 12);
    report.declaredWidth = width;
    report.declaredHeight = height;
    if (width != kExpectedCanvasWidth ||
        height != kExpectedCanvasHeight)
    {
        // Pre-allocation geometry gate (design 4.3 fail-closed): the
        // canvas is never allocated for a non-frozen geometry.
        const bool widthMismatch = width != kExpectedCanvasWidth;
        const std::size_t offset = widthMismatch ? 8u : 12u;
        SetContainerError(report, "FrameGeometryMismatch", offset,
            std::to_string(widthMismatch ? kExpectedCanvasWidth : kExpectedCanvasHeight),
            std::to_string(widthMismatch ? width : height));
        return false;
    }
    const std::uint32_t pixelBytes =
        ReadU32LittleEndian(input, 16);
    if (pixelBytes != kExpectedCanvasWidth * kExpectedCanvasHeight * 4u)
    {
        SetContainerError(report, "InvalidInput", 16,
            std::to_string(kCanvasAllocationBytes), std::to_string(pixelBytes));
        return false;
    }
    for (std::size_t i = 20; i < 28; i++)
    {
        if (input[i] != std::byte{0})
        {
            SetContainerError(report, "NonZeroReservedByte", i, "0x00", ByteHex(input[i]));
            return false;
        }
    }
    const std::size_t expectedTotalBytes =
        kHeaderBytes + kCanvasAllocationBytes;
    if (input.size() < expectedTotalBytes)
    {
        SetContainerError(report, "TruncatedInput", input.size(), "byte", "<eof>");
        return false;
    }
    if (input.size() > expectedTotalBytes)
    {
        SetContainerError(report, "TrailingBytes", expectedTotalBytes,
            "<eof>", ByteHex(input[expectedTotalBytes]));
        return false;
    }
    // The pre-checks guarantee the exact frozen allocation size.
    outCanvas.resize(kCanvasAllocationBytes);
    report.canvasAllocationBytes = kCanvasAllocationBytes;
    std::uint32_t decodedWidth = 0;
    std::uint32_t decodedHeight = 0;
    const auto status = pbmodulation::DecodeRawFrame(
        input, std::span<std::byte>(outCanvas), decodedWidth,
        decodedHeight);
    if (!status)
    {
        SetContainerError(report, ModulationErrorCodeName(status.Error().code),
            status.Error().offset, "valid-pbrw", "decode-failure");
        return false;
    }
    report.containerOk = true;
    return true;
}

// PNG container gate: the PNG signature and the IHDR geometry are
// pre-parsed (IHDR width/height are big-endian at byte offsets 16/20)
// before any canvas allocation.
bool InspectPngContainer(
    const std::span<const std::byte> input,
    InspectorReport& report,
    std::vector<std::byte>& outCanvas)
{
    report.containerType = "png";
    if (input.size() < 8)
    {
        SetContainerError(report, "TruncatedInput", input.size(), "byte", "<eof>");
        return false;
    }
    if (!BytesEqual(input.first(8), kPngSignature))
    {
        SetContainerError(report, "PngDecodeError", 0, "png-signature", ByteHex(input[0]));
        return false;
    }
    // IHDR: 4-byte length (BE, must be 13) + 4-byte type at 8..16,
    // width (BE) at 16, height (BE) at 20.
    if (input.size() < 24)
    {
        SetContainerError(report, "TruncatedInput", input.size(), "byte", "<eof>");
        return false;
    }
    if (ReadU32BigEndian(input, 8) != 13 ||
        !BytesEqual(input.subspan(12, 4),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>("IHDR"), 4)))
    {
        SetContainerError(report, "PngDecodeError", 8, "IHDR-length-and-type", "invalid");
        return false;
    }
    const std::uint32_t width = ReadU32BigEndian(input, 16);
    const std::uint32_t height = ReadU32BigEndian(input, 20);
    report.declaredWidth = width;
    report.declaredHeight = height;
    if (width != kExpectedCanvasWidth ||
        height != kExpectedCanvasHeight)
    {
        const bool widthMismatch = width != kExpectedCanvasWidth;
        const std::size_t offset = widthMismatch ? 16u : 20u;
        SetContainerError(report, "FrameGeometryMismatch", offset,
            std::to_string(widthMismatch ? kExpectedCanvasWidth : kExpectedCanvasHeight),
            std::to_string(widthMismatch ? width : height));
        return false;
    }
    if (input.size() < 33)
    {
        SetContainerError(report, "TruncatedInput", input.size(), "complete-IHDR", "<eof>");
        return false;
    }
    constexpr std::array<std::uint8_t, 5> kExpectedPngParameters{8, 6, 0, 0, 0};
    for (std::size_t parameterIndex = 0; parameterIndex < kExpectedPngParameters.size(); parameterIndex++)
    {
        const std::size_t offset = 24 + parameterIndex;
        const std::uint8_t actual = std::to_integer<std::uint8_t>(input[offset]);
        if (actual != kExpectedPngParameters[parameterIndex])
        {
            SetContainerError(report, "UnsupportedPngFormat", offset,
                ByteHex(std::byte{kExpectedPngParameters[parameterIndex]}), ByteHex(input[offset]));
            return false;
        }
    }
    outCanvas.resize(kCanvasAllocationBytes);
    report.canvasAllocationBytes = kCanvasAllocationBytes;
    const auto status = pbmodulation::DecodePngFrame(
        input, kExpectedCanvasWidth, kExpectedCanvasHeight,
        std::span<std::byte>(outCanvas));
    if (!status)
    {
        SetContainerError(report, ModulationErrorCodeName(status.Error().code),
            status.Error().offset, "valid-png", "decode-failure");
        return false;
    }
    report.containerOk = true;
    return true;
}

// Demodulates the canvas and fills bootstrap / control / data sections.
// On success outBootstrap holds the 44-byte bootstrap record and
// outData the 56,168-byte data region (needed by the recovery chain);
// on failure the buffers are zero-initialized and must not be used.
void InspectDecodedFrame(const std::span<const std::byte> canvas,
    InspectorReport& report,
    std::array<std::byte,
        pbmodulation::kReferenceBootstrapRecordBytes>& outBootstrap,
    std::vector<std::byte>& outData)
{
    auto& bootstrap = outBootstrap;
    std::array<std::byte,
        pbmodulation::kReferenceControlWindowBytes> control{};
    auto& data = outData;
    data.assign(pbmodulation::kReferenceDataRegionBytes, std::byte{0});
    const auto status = pbmodulation::DecodeReferenceFrameInto(
        canvas, std::span<std::byte>(bootstrap),
        std::span<std::byte>(control), std::span<std::byte>(data));
    if (!status)
    {
        report.frameError = ModulationErrorCodeName(status.Error().code);
        report.frameErrorOffset = status.Error().offset;
        AddDiagnostic(report, "canvas-pixel", status.Error().offset,
            "valid-reference-symbol", "invalid-symbol");
        return;
    }
    report.rasterDemodulated = true;

    // Bootstrap section: raw fields + CRC recompute + authoritative
    // parse (the raster layer treats the record as opaque).
    report.bootstrapSessionTag = ReadU64LittleEndian(bootstrap, 16);
    report.bootstrapFrameSequence = ReadU64LittleEndian(bootstrap, 24);
    report.bootstrapControlEpoch =
        ReadU32LittleEndian(bootstrap, 32);
    report.bootstrapFlags = ReadU32LittleEndian(bootstrap, 36);
    report.bootstrapVisualProfileId =
        ReadU64LittleEndian(bootstrap, 8);
    report.bootstrapCrcStored = ReadU32LittleEndian(bootstrap, 40);
    report.bootstrapCrcRecomputed = static_cast<std::uint32_t>(
        pbprotocol::ComputeCrc32c(std::span(bootstrap).first(40)));
    const auto bootstrapResult =
        pbprotocol::ParseBootstrapRecord(bootstrap);
    if (bootstrapResult)
    {
        report.bootstrapParsed = true;
    }
    else
    {
        report.bootstrapError =
            ProtocolErrorCodeName(bootstrapResult.Error().code);
        report.bootstrapErrorOffset = bootstrapResult.Error().offset;
        report.frameError = "Bootstrap:" + report.bootstrapError;
        report.frameErrorOffset = report.bootstrapErrorOffset;
        std::string expected = "valid-bootstrap-byte";
        std::string actual = report.bootstrapErrorOffset < bootstrap.size()
            ? ByteHex(bootstrap[report.bootstrapErrorOffset]) : "<eof>";
        if (bootstrapResult.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch)
        {
            expected = "0x" + ToHexLower(static_cast<std::uint64_t>(report.bootstrapCrcRecomputed)).substr(8);
            actual = "0x" + ToHexLower(static_cast<std::uint64_t>(report.bootstrapCrcStored)).substr(8);
        }
        AddDiagnostic(report, "bootstrap", report.bootstrapErrorOffset, expected, actual);
        return;
    }
    report.frameOk = true;

    // Control window: DIAGNOSTIC semantics. Records are parsed with the
    // envelope-only validator; failures are reported and never fail the
    // frame. The canonical window ends in zero padding.
    std::size_t offset = 0;
    int recordIndex = 0;
    bool stoppedAtZeroPadding = false;
    while (offset < pbmodulation::kReferenceControlWindowBytes)
    {
        if (IsAllZero(std::span(control).subspan(offset)))
        {
            stoppedAtZeroPadding = true;
            break;
        }
        const std::size_t remaining =
            pbmodulation::kReferenceControlWindowBytes - offset;
        std::string line = "[control] record" +
            std::to_string(recordIndex) + " offset=" +
            std::to_string(offset);
        if (remaining < 30 ||
            !BytesEqual(std::span(control).subspan(offset, 4),
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>("PBCR"), 4)))
        {
            report.controlDiagnostics.push_back(
                line + " status=InvalidMagic");
            AddDiagnostic(report, "control-window", offset,
                "PBCR", remaining == 0 ? "<eof>" : ByteHex(control[offset]));
            report.controlTailStatus = "unparseable-tail";
            break;
        }
        const std::uint32_t recordBytes =
            ReadU32LittleEndian(control, offset + 22);
        if (recordBytes < 30 || recordBytes > 65536 ||
            recordBytes > remaining)
        {
            report.controlDiagnostics.push_back(
                line + " size=" + std::to_string(recordBytes) +
                " status=InvalidRecordSize");
            AddDiagnostic(report, "control-window", offset + 22,
                "recordBytes-within-window", std::to_string(recordBytes));
            report.controlTailStatus = "unparseable-tail";
            break;
        }
        line += " size=" + std::to_string(recordBytes);
        const auto recordResult = pbprotocol::ParseControlRecord(
            std::span(control).subspan(offset, recordBytes));
        if (!recordResult)
        {
            const std::size_t innerOffset = recordResult.Error().offset;
            report.controlDiagnostics.push_back(
                line + " status=" +
                ProtocolErrorCodeName(recordResult.Error().code) +
                " errorOffset=" +
                std::to_string(innerOffset));
            if (recordResult.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch && recordBytes >= 4)
            {
                const std::size_t crcOffset = offset + recordBytes - 4;
                const std::uint32_t stored = ReadU32LittleEndian(control, crcOffset);
                const std::uint32_t recomputed = static_cast<std::uint32_t>(
                    pbprotocol::ComputeCrc32c(std::span(control).subspan(offset, recordBytes - 4)));
                AddDiagnostic(report, "control-window", crcOffset,
                    "0x" + ToHexLower(static_cast<std::uint64_t>(recomputed)).substr(8),
                    "0x" + ToHexLower(static_cast<std::uint64_t>(stored)).substr(8));
            }
            else
            {
                const std::size_t errorOffset = offset + innerOffset;
                AddDiagnostic(report, "control-window", errorOffset,
                    "valid-control-byte", errorOffset < control.size()
                        ? ByteHex(control[errorOffset]) : "<eof>");
            }
            offset += recordBytes;
            recordIndex++;
            continue;
        }
        const auto& record = recordResult.Value();
        report.controlRecordCount++;
        report.controlDiagnostics.push_back(
            line + " status=Success type=" +
            std::to_string(
                static_cast<unsigned int>(
                    static_cast<std::underlying_type_t<
                        pbprotocol::ControlRecordType>>(
                        record.recordType))) +
            " tag=0x" + ToHexLower(record.sessionTag.value));
        offset += recordBytes;
        recordIndex++;
    }
    if (stoppedAtZeroPadding ||
        offset == pbmodulation::kReferenceControlWindowBytes)
    {
        report.controlTailStatus = stoppedAtZeroPadding
            ? "zero-padding"
            : "records-fill-window";
    }

    // Data region digest + trailing-zero length.
    report.dataBlake3 =
        pbprotocol::ComputeBlake3Digest(std::span<const std::byte>(data));
    std::size_t trailing = 0;
    for (std::size_t i = data.size();
         i > 0 && data[i - 1] == std::byte{0}; i--)
    {
        trailing = data.size() - i + 1;
    }
    report.dataTrailingZeroBytes = trailing;
}

} // namespace


namespace {

// --recovery chain (fail-closed). Scans the 56,168-byte data region in
// 2,025-byte Robust codeword windows: an all-zero window is the canonical
// padding stop; a non-zero window must pass the syndrome, a bounded
// perfect-LLR decode, info-block extraction and the strict Transport
// parse, and its SessionTag must equal the bootstrap record tag.
void RunRecovery(
    const std::array<std::byte,
        pbmodulation::kReferenceBootstrapRecordBytes>& bootstrapRecord,
    const std::span<const std::byte> data, InspectorReport& report)
{
    report.recoveryRequested = true;
    report.recoveryRan = true;
    const std::uint64_t expectedSessionTag =
        ReadU64LittleEndian(bootstrapRecord, 16);
    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(
        pbinnerfec::kInnerFecProfileIdRobust);
    if (!decoderResult)
    {
        report.recoveryOk = false;
        report.recoveryError = "DecoderCreateFailure";
        return;
    }
    pbinnerfec::InnerFecDecodeOptions options;
    options.maxIterations = 16;
    options.syndromeCheckInterval = 1;
    options.offset = 2048;
    options.scaleNum = 1;
    options.scaleDen = 1;
    auto& decoder = decoderResult.Value();
    std::vector<std::int16_t> llr(pbinnerfec::kDvbS2ShortFrameNBits);
    std::vector<std::byte> decodedCodeword(kRobustCodewordBytes);
    std::vector<std::byte> reencodedCodeword(kRobustCodewordBytes);
    std::size_t windowIndex = 0;
    std::size_t windowByteOffset = 0;
    while (windowByteOffset < kDataRegionBytes)
    {
        const std::size_t remainingBytes =
            kDataRegionBytes - windowByteOffset;
        const std::size_t windowBytes =
            remainingBytes < kRobustCodewordBytes
                ? remainingBytes
                : kRobustCodewordBytes;
        const std::span<const std::byte> window =
            data.subspan(windowByteOffset, windowBytes);
        if (IsAllZero(window))
        {
            const std::span<const std::byte> padding =
                data.subspan(windowByteOffset);
            const auto firstNonZero = std::find_if(
                padding.begin(), padding.end(),
                [](const std::byte value)
                {
                    return value != std::byte{0};
                });
            if (firstNonZero != padding.end())
            {
                const std::size_t innerOffset = static_cast<std::size_t>(
                    firstNonZero - padding.begin());
                report.recoveryOk = false;
                report.recoveryError = "NonCanonicalPadding";
                report.recoveryErrorOffset = windowByteOffset;
                AddRecoveryDiagnostic(report, windowByteOffset, innerOffset,
                    "0x00", ByteHex(*firstNonZero));
                return;
            }
            report.recoveryStoppedAtPadding = true;
            break;
        }
        report.recoveryWindowCount++;
        if (windowBytes < kRobustCodewordBytes)
        {
            report.recoveryOk = false;
            report.recoveryError = "TruncatedCodeword";
            report.recoveryErrorOffset = windowByteOffset;
            AddRecoveryDiagnostic(report, windowByteOffset, windowBytes,
                "2025-byte-codeword", "<eof>");
            return;
        }
        const auto syndromeResult =
            pbinnerfec::ComputeQcLdpcSyndrome(
                pbinnerfec::kInnerFecProfileIdRobust, window);
        if (!syndromeResult || !syndromeResult.Value())
        {
            report.recoveryOk = false;
            report.recoveryError =
                syndromeResult ? "SyndromeFailure" : "SyndromeCheckError";
            report.recoveryErrorOffset = windowByteOffset;
            AddRecoveryDiagnostic(report, windowByteOffset,
                0,
                "zero-syndrome", syndromeResult ? "nonzero-syndrome" : "syndrome-error");
            return;
        }
        // Perfect-LLR soft decisions for the clean-channel reference
        // decode: bit == 0 -> +100, bit == 1 -> -100 (LLR convention:
        // >0 decides 0).
        for (std::size_t byteIndex = 0;
            byteIndex < kRobustCodewordBytes; byteIndex++)
        {
            const std::uint8_t bits = std::to_integer<std::uint8_t>(
                window[byteIndex]);
            for (std::size_t bitIndex = 0; bitIndex < 8; bitIndex++)
            {
                const bool bitSet =
                    ((bits >> bitIndex) & 0x1u) != 0u;
                llr[byteIndex * 8u + bitIndex] = bitSet
                    ? -kPerfectLlrMagnitude
                    : kPerfectLlrMagnitude;
            }
        }
        const auto decodeResult = decoder.Decode(
            std::span<const std::int16_t>(llr), options,
            std::span<std::byte>(decodedCodeword));
        if (!decodeResult)
        {
            report.recoveryOk = false;
            report.recoveryError = "DecodeFailure";
            report.recoveryErrorOffset = windowByteOffset;
            AddRecoveryDiagnostic(report, windowByteOffset,
                0, "clean-channel-decode", "decode-error");
            return;
        }
        if (!BytesEqual(
            std::span<const std::byte>(decodedCodeword), window))
        {
            report.recoveryOk = false;
            report.recoveryError = "DecodeMismatch";
            report.recoveryErrorOffset = windowByteOffset;
            const auto mismatch = std::mismatch(decodedCodeword.begin(), decodedCodeword.end(), window.begin());
            const std::size_t innerOffset = static_cast<std::size_t>(mismatch.first - decodedCodeword.begin());
            AddRecoveryDiagnostic(report, windowByteOffset, innerOffset,
                ByteHex(window[innerOffset]), ByteHex(decodedCodeword[innerOffset]));
            return;
        }
        const auto reencodeStatus = pbinnerfec::EncodeQcLdpcCodeword(
            pbinnerfec::kInnerFecProfileIdRobust,
            std::span<const std::byte>(decodedCodeword).first(kRobustInfoBlockBytes),
            std::span<std::byte>(reencodedCodeword));
        if (!reencodeStatus)
        {
            report.recoveryOk = false;
            report.recoveryError = "ReencodeFailure";
            report.recoveryErrorOffset = windowByteOffset;
            AddRecoveryDiagnostic(report, windowByteOffset, 0,
                "reencodable-info-block", "encode-error");
            return;
        }
        if (!BytesEqual(std::span<const std::byte>(reencodedCodeword),
            std::span<const std::byte>(decodedCodeword)))
        {
            report.recoveryOk = false;
            report.recoveryError = "ReencodeMismatch";
            report.recoveryErrorOffset = windowByteOffset;
            const auto mismatch = std::mismatch(reencodedCodeword.begin(), reencodedCodeword.end(), decodedCodeword.begin());
            const std::size_t innerOffset = static_cast<std::size_t>(mismatch.first - reencodedCodeword.begin());
            AddRecoveryDiagnostic(report, windowByteOffset, innerOffset,
                ByteHex(decodedCodeword[innerOffset]), ByteHex(reencodedCodeword[innerOffset]));
            return;
        }
        const auto extractedBlock =
            pbprotocol::ExtractTransportBlockFromInfoBlock(
                std::span<const std::byte>(decodedCodeword).first(
                    kRobustInfoBlockBytes));
        if (!extractedBlock)
        {
            report.recoveryOk = false;
            report.recoveryError = "ExtractionFailure:" +
                ProtocolErrorCodeName(extractedBlock.Error().code);
            report.recoveryErrorOffset = windowByteOffset;
            const std::size_t innerOffset = extractedBlock.Error().offset;
            AddRecoveryDiagnostic(report, windowByteOffset, innerOffset,
                "canonical-info-block", innerOffset < kRobustInfoBlockBytes
                    ? ByteHex(decodedCodeword[innerOffset]) : "<eof>");
            return;
        }
        const std::span<const std::byte> blockSpan =
            extractedBlock.Value();
        const auto parsedBlock =
            pbprotocol::ParseTransportBlock(blockSpan);
        if (!parsedBlock)
        {
            report.recoveryOk = false;
            report.recoveryError = "TransportParseFailure:" +
                ProtocolErrorCodeName(parsedBlock.Error().code);
            report.recoveryErrorOffset = windowByteOffset;
            const std::size_t innerOffset = parsedBlock.Error().offset;
            AddRecoveryDiagnostic(report, windowByteOffset, innerOffset,
                "valid-transport-block", innerOffset < blockSpan.size()
                    ? ByteHex(blockSpan[innerOffset]) : "<eof>");
            return;
        }
        const auto& block = parsedBlock.Value();
        if (block.header.sessionTag.value != expectedSessionTag)
        {
            report.recoveryOk = false;
            report.recoveryError = "SessionTagMismatch";
            report.recoveryErrorOffset = windowByteOffset;
            report.sessionTagCrossCheckOk = false;
            AddRecoveryDiagnostic(report, windowByteOffset, 4,
                "0x" + ToHexLower(expectedSessionTag),
                "0x" + ToHexLower(block.header.sessionTag.value));
            return;
        }
        RecoveredBlock recovered;
        recovered.windowIndex = windowIndex;
        recovered.windowByteOffset = windowByteOffset;
        recovered.sessionTag = block.header.sessionTag.value;
        recovered.segmentOrdinal = block.header.segmentOrdinal;
        recovered.outerBlockId = block.header.outerBlockId;
        recovered.payloadBytes = block.payload.size();
        recovered.payloadCrcStored =
            ReadU32LittleEndian(blockSpan, blockSpan.size() - 4);
        recovered.payloadCrcRecomputed = static_cast<std::uint32_t>(
            pbprotocol::ComputeCrc32c(block.payload));
        recovered.payloadBlake3 =
            pbprotocol::ComputeBlake3Digest(block.payload);
        report.blocks.push_back(std::move(recovered));
        windowIndex++;
        windowByteOffset += kRobustCodewordBytes;
    }
}

} // namespace

void InspectFrame(const std::span<const std::byte> input,
    const bool recoveryRequested, InspectorReport& outReport)
{
    InspectorReport report;
    report.fileBytes = input.size();
    report.recoveryRequested = recoveryRequested;
    try
    {
        FillRegionTable(report);
        std::vector<std::byte> canvas;
        bool canvasReady = false;
        if (input.size() >= 4 &&
            BytesEqual(input.first(4),
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>("PBRW"), 4)))
        {
            canvasReady = InspectPbrwContainer(input, report, canvas);
        }
        else if (input.size() >= 8 &&
            BytesEqual(input.first(8), std::span(kPngSignature)))
        {
            canvasReady = InspectPngContainer(input, report, canvas);
        }
        else
        {
            report.containerType = "unrecognized";
            const std::string actual = input.empty() ? "<eof>" : ByteHex(input[0]);
            SetContainerError(report, "InvalidMagic", 0, "PBRW-or-PNG", actual);
        }
        if (!canvasReady)
        {
            outReport = std::move(report);
            return;
        }
        std::array<std::byte,
            pbmodulation::kReferenceBootstrapRecordBytes> bootstrap{};
        std::vector<std::byte> data;
        InspectDecodedFrame(std::span<const std::byte>(canvas), report,
            bootstrap, data);
        if (!report.frameOk)
        {
            outReport = std::move(report);
            return;
        }
        if (recoveryRequested && report.bootstrapParsed)
        {
            RunRecovery(bootstrap, std::span<const std::byte>(data), report);
        }
    }
    catch (const std::bad_alloc&)
    {
        report.containerOk = false;
        report.frameOk = false;
        report.containerError = "MemoryAllocationFailure";
        report.containerErrorOffset = 0;
    }
    catch (const std::exception&)
    {
        report.containerOk = false;
        report.frameOk = false;
        report.containerError = "InternalInvariantViolation";
        report.containerErrorOffset = 0;
    }
    catch (...)
    {
        report.containerOk = false;
        report.frameOk = false;
        report.containerError = "InternalInvariantViolation";
        report.containerErrorOffset = 0;
    }
    outReport = std::move(report);
}

std::string FormatReport(const InspectorReport& report)
{
    std::string out;
    out += "[container] type=" + report.containerType +
        " bytes=" + std::to_string(report.fileBytes) +
        " status=" +
        (report.containerOk ? "ok" : report.containerError);
    if (!report.containerOk)
    {
        out += " offset=" + std::to_string(report.containerErrorOffset);
    }
    out += "\n";
    out += "[canvas] declaredWidth=" +
        std::to_string(report.declaredWidth) +
        " declaredHeight=" + std::to_string(report.declaredHeight) +
        " allocationBytes=" +
        std::to_string(report.canvasAllocationBytes) + "\n";
    if (report.containerOk)
    {
        // Frame-level status only exists once the container gate passed;
        // on container failure the error is already reported above.
        out += "[frame] status=" +
            (report.frameOk ? "ok" : report.frameError);
        if (!report.frameOk)
        {
            out += " offset=" + std::to_string(report.frameErrorOffset);
        }
        out += "\n";
    }
    for (const auto& region : report.regions)
    {
        out += "[region] index=" + std::to_string(region.index) +
            " type=" + region.typeName +
            " x=" + std::to_string(region.x) +
            " y=" + std::to_string(region.y) +
            " width=" + std::to_string(region.width) +
            " height=" + std::to_string(region.height) +
            " pixelOffset=" + std::to_string(region.pixelOffset) +
            " pixelBytes=" + std::to_string(region.pixelBytes) + "\n";
    }
    if (report.rasterDemodulated)
    {
        out += "[bootstrap] status=" +
            (report.bootstrapParsed ? "Success"
                                    : report.bootstrapError);
        if (!report.bootstrapParsed)
        {
            out += " offset=" +
                std::to_string(report.bootstrapErrorOffset);
        }
        out += " crc=";
        out += report.bootstrapCrcStored == report.bootstrapCrcRecomputed
            ? "match"
            : "mismatch";
        out += "\n";
        out += "[bootstrap] sessionTag=0x" +
            ToHexLower(report.bootstrapSessionTag) +
            " frameSequence=0x" +
            ToHexLower(report.bootstrapFrameSequence) +
            " controlEpoch=0x" +
            ToHexLower(report.bootstrapControlEpoch) +
            " flags=0x" + ToHexLower(report.bootstrapFlags) +
            " visualProfileId=0x" +
            ToHexLower(report.bootstrapVisualProfileId) + "\n";
    }
    if (report.frameOk)
    {
        out += "[control] windowBytes=240 records=" +
            std::to_string(report.controlRecordCount) +
            " tail=" + report.controlTailStatus + "\n";
        for (const auto& line : report.controlDiagnostics)
        {
            out += line + "\n";
        }
        out += "[data] blake3=" + ToHexLower(report.dataBlake3) +
            " trailingZeroBytes=" +
            std::to_string(report.dataTrailingZeroBytes) + "\n";
    }
    out += "[interleave] status=not-bound\n";
    if (report.recoveryRequested)
    {
        out += "[recovery] windows=" +
            std::to_string(report.recoveryWindowCount) +
            " stoppedAtPadding=" +
            (report.recoveryStoppedAtPadding ? "1" : "0") +
            " status=" +
            (!report.recoveryRan ? "not-run" :
                (report.recoveryOk ? "ok" : report.recoveryError));
        if (report.recoveryRan && !report.recoveryOk)
        {
            out += " offset=" +
                std::to_string(report.recoveryErrorOffset);
        }
        out += " sessionTagCrossCheck=";
        out += !report.recoveryRan ? "not-run" :
            (report.sessionTagCrossCheckOk ? "ok" : "mismatch");
        out += "\n";
        for (const auto& block : report.blocks)
        {
            out += "[recovery] block" +
                std::to_string(block.windowIndex) +
                " offset=" +
                std::to_string(block.windowByteOffset) +
                " tag=0x" + ToHexLower(block.sessionTag) +
                " ordinal=" + std::to_string(block.segmentOrdinal) +
                " blockId=" + std::to_string(block.outerBlockId) +
                " payloadBytes=" +
                std::to_string(block.payloadBytes) +
                " payloadCrc=" +
                (block.payloadCrcStored == block.payloadCrcRecomputed
                    ? "match"
                    : "mismatch") +
                " payloadBlake3=" + ToHexLower(block.payloadBlake3) + "\n";
        }
    }
    for (const FailureDiagnostic& diagnostic : report.diagnostics)
    {
        out += "[diagnostic] space=" + diagnostic.space +
            " byte_offset=" + std::to_string(diagnostic.byteOffset) +
            " expected=" + diagnostic.expected +
            " actual=" + diagnostic.actual;
        if (diagnostic.hasNestedOffsets)
        {
            out += " windowByteOffset=" + std::to_string(diagnostic.windowByteOffset) +
                " innerByteOffset=" + std::to_string(diagnostic.innerByteOffset) +
                " dataRegionByteOffset=" + std::to_string(diagnostic.dataRegionByteOffset);
        }
        out += "\n";
    }
    return out;
}

} // namespace pbinspect
