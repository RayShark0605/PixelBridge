#include "fixtures.h"
#include "unified_point_downscale_fixture.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <stdexcept>

namespace pbg1
{
void Require(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

pbmodulation::LumaView PixelFrame::View() const noexcept
{
    return {pixels, width, height, static_cast<std::size_t>(width) * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

Fixture BuildFixture(const std::uint64_t sequence)
{
    using namespace pbmodulation;
    Fixture fixture;
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bootstrap{};
    // Deterministic test identity, not an application Session or source-file handoff.
    const pbprotocol::SessionTag sessionTag{0x1122334455667788ULL};
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        kUnifiedVisualProfile.productProfile.visualLayoutVersion, kUnifiedVisualProfile.productProfile.visualProfileId,
        sessionTag, sequence, 0x21222324U, 0};
    Require(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(record, bootstrap)), "Bootstrap fixture serialization");
    std::array<std::byte, kUnifiedCodedFrameBytes> coded{};
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        std::vector<std::byte> payload(48 + slot % 13);
        for (std::size_t index = 0; index < payload.size(); index++)
        {
            payload[index] = static_cast<std::byte>((slot * 37 + index * 11 + 5) & 0xFFU);
        }
        const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData, pbprotocol::kTransportProtocolMinor,
            0, sessionTag, 900 + slot, 1000 + slot, static_cast<std::uint16_t>(payload.size())};
        auto& block = fixture.expectedBlocks[slot];
        block.resize(pbprotocol::GetTransportSerializedSize(header));
        Require(static_cast<bool>(pbprotocol::SerializeTransportBlock(header, payload, block)), "Transport fixture serialization");
        std::array<std::byte, kUnifiedInformationBytes> information{};
        Require(static_cast<bool>(pbprotocol::FrameTransportBlockIntoInfoBlock(block, information.size(), information)), "Info block fixture framing");
        Require(static_cast<bool>(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
            std::span(coded).subspan(static_cast<std::size_t>(slot) * kUnifiedCodewordBytes, kUnifiedCodewordBytes))), "Inner FEC fixture encoding");
    }
    fixture.frame.pixels.resize(kUnifiedFrameBgraBytes);
    Require(static_cast<bool>(EncodeUnifiedVisualFrame(bootstrap, coded, fixture.frame.pixels)), "Production fixture raster");
    return fixture;
}

PixelFrame CropEdge(const PixelFrame& source, const std::uint32_t edge)
{
    Require(source.width >= 2 && source.height >= 2 && edge < 4, "Invalid bounded crop fixture");
    PixelFrame result;
    result.width = source.width - static_cast<std::uint32_t>(edge < 2);
    result.height = source.height - static_cast<std::uint32_t>(edge >= 2);
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    const std::uint32_t sourceX = edge == 0 ? 1 : 0;
    const std::uint32_t sourceY = edge == 2 ? 1 : 0;
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        std::copy_n(source.pixels.data() + (static_cast<std::size_t>(row + sourceY) * source.width + sourceX) * 4,
            static_cast<std::size_t>(result.width) * 4, result.pixels.data() + static_cast<std::size_t>(row) * result.width * 4);
    }
    return result;
}

PixelFrame PadEdge(const PixelFrame& cropped, const std::uint32_t edge)
{
    Require(cropped.width <= 1920 && cropped.height <= 1080 && edge < 4, "Invalid bounded padding fixture");
    PixelFrame result;
    result.width = cropped.width + static_cast<std::uint32_t>(edge < 2);
    result.height = cropped.height + static_cast<std::uint32_t>(edge >= 2);
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    for (std::size_t offset = 0; offset < result.pixels.size(); offset += 4)
    {
        std::fill_n(result.pixels.data() + offset, 3, static_cast<std::byte>(pbmodulation::kLocalDesktopBackground));
        result.pixels[offset + 3] = std::byte{255};
    }
    const std::uint32_t targetX = edge == 0 ? 1 : 0;
    const std::uint32_t targetY = edge == 2 ? 1 : 0;
    for (std::uint32_t row = 0; row < cropped.height; row++)
    {
        std::copy_n(cropped.pixels.data() + static_cast<std::size_t>(row) * cropped.width * 4,
            static_cast<std::size_t>(cropped.width) * 4, result.pixels.data() + (static_cast<std::size_t>(row + targetY) * result.width + targetX) * 4);
    }
    return result;
}

PixelFrame OffsetIntensity(const PixelFrame& source)
{
    PixelFrame result = source;
    for (std::size_t offset = 0; offset < result.pixels.size(); offset++)
    {
        if (offset % 4 != 3)
        {
            const auto value = std::to_integer<unsigned>(result.pixels[offset]);
            result.pixels[offset] = static_cast<std::byte>(std::min(255U, value + 1));
        }
    }
    return result;
}

PixelFrame BlurAxis(const PixelFrame& source, const bool horizontal)
{
    PixelFrame result = source;
    for (std::uint32_t row = 1; row + 1 < source.height; row++)
    {
        for (std::uint32_t column = 1; column + 1 < source.width; column++)
        {
            const std::size_t centre = (static_cast<std::size_t>(row) * source.width + column) * 4;
            const std::size_t delta = horizontal ? 4 : static_cast<std::size_t>(source.width) * 4;
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                const auto total = std::to_integer<unsigned>(source.pixels[centre - delta + channel]) +
                    2 * std::to_integer<unsigned>(source.pixels[centre + channel]) + std::to_integer<unsigned>(source.pixels[centre + delta + channel]);
                result.pixels[centre + channel] = static_cast<std::byte>((total + 2) / 4);
            }
        }
    }
    return result;
}

PixelFrame DownscalePoint(const PixelFrame& source)
{
    PixelFrame result;
    result.width = source.width * 3 / 4;
    result.height = source.height * 3 / 4;
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            const auto sourceX = (2 * column + 1) * 2 / 3;
            const auto sourceY = (2 * row + 1) * 2 / 3;
            std::copy_n(source.pixels.data() + (static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4, 4,
                result.pixels.data() + (static_cast<std::size_t>(row) * result.width + column) * 4);
        }
    }
    return result;
}

PixelFrame UpscalePoint(const PixelFrame& source)
{
    Require(source.width == 1920 && source.height == 1080, "Upscale requires canonical fixture");
    return {2160, 1215, pbtest::UpscaleUnifiedPoint(source.pixels, false, false)};
}

PixelFrame ReplaceRegion(const PixelFrame& source, const PixelFrame& donor, const pbmodulation::LocalDesktopRegion& region)
{
    Require(source.width == donor.width && source.height == donor.height && source.pixels.size() == donor.pixels.size(), "Fixture region dimensions");
    Require(region.x <= source.width && region.width <= source.width - region.x && region.y <= source.height && region.height <= source.height - region.y, "Fixture region bounds");
    PixelFrame result = source;
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        const std::size_t offset = (static_cast<std::size_t>(region.y + row) * source.width + region.x) * 4;
        std::copy_n(donor.pixels.data() + offset, static_cast<std::size_t>(region.width) * 4, result.pixels.data() + offset);
    }
    return result;
}

PixelFrame PerturbMarkerEdge(const PixelFrame& source)
{
    PixelFrame result = source;
    const auto& marker = pbmodulation::kLocalDesktopMarkerRegions[0];
    const std::uint32_t column = marker.x + marker.width / 2 - 28;
    const std::uint32_t row = marker.y + marker.height / 2 - 1;
    const std::size_t offset = (static_cast<std::size_t>(row) * source.width + column) * 4;
    for (std::size_t channel = 0; channel < 3; channel++)
    {
        const auto value = std::to_integer<unsigned>(result.pixels[offset + channel]);
        result.pixels[offset + channel] = static_cast<std::byte>(std::min(255U, value + 1));
    }
    return result;
}

PixelFrame EraseBootstrap(const PixelFrame& source)
{
    PixelFrame result = source;
    for (const auto& region : pbmodulation::kLocalDesktopBootstrapRegions)
    {
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const std::size_t offset = (static_cast<std::size_t>(region.y + row) * source.width + region.x + column) * 4;
                std::fill_n(result.pixels.data() + offset, 3, std::byte{128});
            }
        }
    }
    return result;
}
}
