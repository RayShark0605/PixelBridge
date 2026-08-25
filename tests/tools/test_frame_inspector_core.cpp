#include "frame_inspector_core.h"

#include "pbgolden/golden_vector_source.h"
#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

enum class ContainerFormat
{
    Pbrw,
    Png
};

[[nodiscard]] std::vector<std::byte> EncodeContainer(
    const pbgolden::GoldenFramePayload& payload, const ContainerFormat format)
{
    std::vector<std::byte> canvas(pbmodulation::kReferenceFrameBgraBytes);
    pbmodulation::ReferenceFrameInput input;
    input.bootstrapRecord = payload.bootstrap;
    input.controlWindow = payload.control;
    input.data = payload.data;
    REQUIRE(pbmodulation::EncodeReferenceFrame(input, canvas));
    const auto encoded = format == ContainerFormat::Pbrw
        ? pbmodulation::EncodeRawFrame(canvas, pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight)
        : pbmodulation::EncodePngFrame(canvas, pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight);
    REQUIRE(encoded);
    return encoded.Value();
}

void StoreU32(std::vector<std::byte>& bytes, const std::size_t offset,
    const std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8u))};
    }
}

[[nodiscard]] pbgolden::GoldenFramePayload MakePayloadFromInfoBlock(
    const std::span<const std::byte> infoBlock)
{
    auto payload = pbgolden::MakeG1TransportFramePayload();
    std::vector<std::byte> codeword(2025);
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(
        pbinnerfec::kInnerFecProfileIdRobust, infoBlock, codeword));
    std::fill(payload.data.begin(), payload.data.end(), std::byte{0});
    std::copy(codeword.begin(), codeword.end(), payload.data.begin());
    return payload;
}

} // namespace

TEST_CASE("PBFrameInspector recovers one and two Transport codewords from raw and PNG",
    "[tools][frame-inspector][integration]")
{
    const auto onePayload = pbgolden::MakeG1TransportFramePayload();
    for (const ContainerFormat format : {ContainerFormat::Pbrw, ContainerFormat::Png})
    {
        const auto container = EncodeContainer(onePayload, format);
        pbinspect::InspectorReport report;
        pbinspect::InspectFrame(container, true, report);
        REQUIRE(report.containerOk);
        REQUIRE(report.rasterDemodulated);
        REQUIRE(report.bootstrapParsed);
        REQUIRE(report.frameOk);
        REQUIRE(report.recoveryRan);
        REQUIRE(report.recoveryOk);
        REQUIRE(report.sessionTagCrossCheckOk);
        REQUIRE(report.blocks.size() == 1);
        REQUIRE(report.recoveryStoppedAtPadding);
        REQUIRE(pbinspect::FormatReport(report).find("[interleave] status=not-bound") !=
            std::string::npos);
    }

    const auto twoPayload = pbgolden::MakeG1Transport2CwFramePayload();
    const auto raw = EncodeContainer(twoPayload, ContainerFormat::Pbrw);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(raw, true, report);
    REQUIRE(report.frameOk);
    REQUIRE(report.recoveryRan);
    REQUIRE(report.recoveryOk);
    REQUIRE(report.blocks.size() == 2);
    REQUIRE(report.blocks[0].segmentOrdinal == 0);
    REQUIRE(report.blocks[1].segmentOrdinal == 1);
}

TEST_CASE("PBFrameInspector rejects hostile containers before canvas allocation",
    "[tools][frame-inspector][bounds]")
{
    std::vector<std::byte> header(pbmodulation::kRawFrameHeaderBytes, std::byte{0});
    header[0] = std::byte{0x50};
    header[1] = std::byte{0x42};
    header[2] = std::byte{0x52};
    header[3] = std::byte{0x57};
    header[4] = std::byte{1};
    StoreU32(header, 8, 16384);
    StoreU32(header, 12, 16384);
    StoreU32(header, 16, 0);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(header, false, report);
    REQUIRE_FALSE(report.containerOk);
    REQUIRE(report.containerError == "FrameGeometryMismatch");
    REQUIRE(report.containerErrorOffset == 8);
    REQUIRE(report.canvasAllocationBytes == 0);
    REQUIRE(report.diagnostics.front().expected == "1920");
    REQUIRE(report.diagnostics.front().actual == "16384");

    header[8] = std::byte{0x80};
    header[9] = std::byte{0x07};
    header[10] = std::byte{0};
    header[11] = std::byte{0};
    header[12] = std::byte{0x38};
    header[13] = std::byte{0x04};
    header[5] = std::byte{1};
    pbinspect::InspectFrame(header, false, report);
    REQUIRE(report.containerError == "NonZeroReservedByte");
    REQUIRE(report.containerErrorOffset == 5);
    REQUIRE(report.canvasAllocationBytes == 0);

    header[5] = std::byte{0};
    StoreU32(header, 16, static_cast<std::uint32_t>(pbinspect::kCanvasAllocationBytes));
    header[23] = std::byte{1};
    pbinspect::InspectFrame(header, false, report);
    REQUIRE(report.containerError == "NonZeroReservedByte");
    REQUIRE(report.containerErrorOffset == 23);
    REQUIRE(report.canvasAllocationBytes == 0);

    std::vector<std::byte> png(33, std::byte{0});
    constexpr std::array<std::uint8_t, 8> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (std::size_t index = 0; index < signature.size(); index++)
    {
        png[index] = std::byte{signature[index]};
    }
    png[11] = std::byte{13};
    png[12] = std::byte{0x49};
    png[13] = std::byte{0x48};
    png[14] = std::byte{0x44};
    png[15] = std::byte{0x52};
    png[16] = std::byte{0};
    png[17] = std::byte{0};
    png[18] = std::byte{0x40};
    png[19] = std::byte{0};
    png[20] = std::byte{0};
    png[21] = std::byte{0};
    png[22] = std::byte{0x40};
    png[23] = std::byte{0};
    pbinspect::InspectFrame(png, false, report);
    REQUIRE(report.containerError == "FrameGeometryMismatch");
    REQUIRE(report.containerErrorOffset == 16);
    REQUIRE(report.canvasAllocationBytes == 0);
}

TEST_CASE("PBFrameInspector makes Bootstrap authoritative and Control diagnostic-only",
    "[tools][frame-inspector]")
{
    auto bootstrapInvalid = pbgolden::MakeG1TransportFramePayload();
    bootstrapInvalid.bootstrap[40] ^= std::byte{1};
    const auto invalidBootstrapRaw = EncodeContainer(bootstrapInvalid, ContainerFormat::Pbrw);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(invalidBootstrapRaw, true, report);
    REQUIRE(report.containerOk);
    REQUIRE(report.rasterDemodulated);
    REQUIRE_FALSE(report.bootstrapParsed);
    REQUIRE_FALSE(report.frameOk);
    REQUIRE_FALSE(report.recoveryRan);
    REQUIRE(report.recoveryWindowCount == 0);
    REQUIRE(report.blocks.empty());
    const std::string invalidBootstrapReport = pbinspect::FormatReport(report);
    REQUIRE(invalidBootstrapReport.find("expected=0x") != std::string::npos);
    REQUIRE(invalidBootstrapReport.find("status=not-run") != std::string::npos);
    REQUIRE(invalidBootstrapReport.find("sessionTagCrossCheck=not-run") !=
        std::string::npos);

    auto unsupportedMajor = pbgolden::MakeG1TransportFramePayload();
    unsupportedMajor.bootstrap[5] = std::byte{2};
    const std::uint32_t bootstrapCrc = static_cast<std::uint32_t>(
        pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(unsupportedMajor.bootstrap).first(40)));
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        unsupportedMajor.bootstrap[40 + byteIndex] = std::byte{
            static_cast<std::uint8_t>(bootstrapCrc >> (byteIndex * 8u))};
    }
    const auto unsupportedMajorRaw = EncodeContainer(
        unsupportedMajor, ContainerFormat::Pbrw);
    pbinspect::InspectFrame(unsupportedMajorRaw, true, report);
    REQUIRE_FALSE(report.frameOk);
    REQUIRE_FALSE(report.bootstrapParsed);
    REQUIRE(report.bootstrapError == "UnsupportedProtocolMajor");
    REQUIRE(report.bootstrapErrorOffset == 5);
    REQUIRE_FALSE(report.recoveryRan);

    auto controlInvalid = pbgolden::MakeG1TransportFramePayload();
    controlInvalid.control[63] ^= std::byte{1};
    const auto invalidControlRaw = EncodeContainer(controlInvalid, ContainerFormat::Pbrw);
    pbinspect::InspectFrame(invalidControlRaw, false, report);
    REQUIRE(report.frameOk);
    REQUIRE(report.bootstrapParsed);
    REQUIRE(report.controlRecordCount == 0);
    REQUIRE(report.controlDiagnostics.size() == 1);
    REQUIRE(report.controlDiagnostics[0].find("CrcMismatch") != std::string::npos);
    REQUIRE_FALSE(report.diagnostics.empty());
    REQUIRE(report.diagnostics.back().space == "control-window");
}

TEST_CASE("PBFrameInspector rejects nonzero data after a zero recovery window",
    "[tools][frame-inspector][recovery][padding]")
{
    auto payload = pbgolden::MakeG1TransportFramePayload();
    std::fill(payload.data.begin(), payload.data.end(), std::byte{0});
    const std::size_t hiddenOffset = pbinspect::kRobustCodewordBytes + 7;
    payload.data[hiddenOffset] = std::byte{0x5A};
    const auto raw = EncodeContainer(payload, ContainerFormat::Pbrw);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(raw, true, report);
    REQUIRE(report.frameOk);
    REQUIRE(report.recoveryRan);
    REQUIRE_FALSE(report.recoveryOk);
    REQUIRE_FALSE(report.recoveryStoppedAtPadding);
    REQUIRE(report.recoveryError == "NonCanonicalPadding");
    REQUIRE(report.recoveryWindowCount == 0);
    REQUIRE(report.diagnostics.back().windowByteOffset == 0);
    REQUIRE(report.diagnostics.back().innerByteOffset == hiddenOffset);
    REQUIRE(report.diagnostics.back().dataRegionByteOffset == hiddenOffset);
    REQUIRE(report.diagnostics.back().expected == "0x00");
    REQUIRE(report.diagnostics.back().actual == "0x5a");
}

TEST_CASE("PBFrameInspector recovery reports nested offsets", "[tools][frame-inspector][recovery]")
{
    auto syndromePayload = pbgolden::MakeG1TransportFramePayload();
    syndromePayload.data[17] ^= std::byte{1};
    auto raw = EncodeContainer(syndromePayload, ContainerFormat::Pbrw);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(raw, true, report);
    REQUIRE(report.frameOk);
    REQUIRE_FALSE(report.recoveryOk);
    REQUIRE(report.recoveryError == "SyndromeFailure");
    REQUIRE_FALSE(report.diagnostics.empty());
    REQUIRE(report.diagnostics.back().hasNestedOffsets);
    REQUIRE(report.diagnostics.back().windowByteOffset == 0);

    std::vector<std::byte> dirtyInfo(1350, std::byte{0});
    const auto minimum = pbgolden::GenerateTransportBlockMinimum();
    std::copy(minimum.begin(), minimum.end(), dirtyInfo.begin());
    dirtyInfo.back() = std::byte{1};
    const auto dirtyPayload = MakePayloadFromInfoBlock(dirtyInfo);
    raw = EncodeContainer(dirtyPayload, ContainerFormat::Pbrw);
    pbinspect::InspectFrame(raw, true, report);
    REQUIRE_FALSE(report.recoveryOk);
    REQUIRE(report.recoveryError.find("NonCanonicalPadding") != std::string::npos);
    REQUIRE(report.diagnostics.back().innerByteOffset == 1349);
    REQUIRE(report.diagnostics.back().dataRegionByteOffset == 1349);
}

TEST_CASE("PBFrameInspector recovery rejects a decoded SessionTag mismatch",
    "[tools][frame-inspector][recovery]")
{
    const auto canonical = pbgolden::GenerateTransportBlockCanonical();
    const auto parsed = pbprotocol::ParseTransportBlock(canonical);
    REQUIRE(parsed);
    auto header = parsed.Value().header;
    header.sessionTag.value++;
    std::vector<std::byte> changed(canonical.size());
    REQUIRE(pbprotocol::SerializeTransportBlock(header, parsed.Value().payload, changed));
    const auto payload = MakePayloadFromInfoBlock(changed);
    const auto raw = EncodeContainer(payload, ContainerFormat::Pbrw);
    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(raw, true, report);
    REQUIRE(report.frameOk);
    REQUIRE_FALSE(report.recoveryOk);
    REQUIRE_FALSE(report.sessionTagCrossCheckOk);
    REQUIRE(report.recoveryError == "SessionTagMismatch");
    REQUIRE(report.diagnostics.back().innerByteOffset == 4);
    REQUIRE(report.diagnostics.back().expected != report.diagnostics.back().actual);
}
