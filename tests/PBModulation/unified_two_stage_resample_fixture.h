#pragma once

#include "unified_fullscreen_fixture.h"

#include "pbmodulation/unified_visual.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/product_visual_profile.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace pbtest
{

// Test-only two-stage presentation chain for the Unified single-monitor
// fullscreen route: canonical 1920x1080 -> full-screen raster (stage one,
// encoder presentation sampling) -> remote-view rescale into the captured
// 2560x1440 ROI (stage two, fractional independent-axis bilinear, the same
// channel model the decoder forward sampling assumes). Point reproduces the
// production center mapping; Linear and Area are unverified candidate
// references for a friendlier remote-codec presentation and change no
// production filter, threshold, or profile contract.
enum class UnifiedFullscreenSamplingMode : std::uint8_t
{
    Point,
    Linear,
    Area
};

inline constexpr std::uint32_t kTwoStageCanonicalWidth = 1920;
inline constexpr std::uint32_t kTwoStageCanonicalHeight = 1080;
inline constexpr std::uint32_t kTwoStageCaptureWidth = 2560;
inline constexpr std::uint32_t kTwoStageCaptureHeight = 1440;

[[nodiscard]] inline bool IsTwoStageFullscreenSize(const std::uint32_t width, const std::uint32_t height) noexcept
{
    return width >= kTwoStageCanonicalWidth && width <= kTwoStageCanonicalWidth * 2U &&
        height >= kTwoStageCanonicalHeight && height <= kTwoStageCanonicalHeight * 2U;
}

// Stage-one bilinear upscale with edge clamping. The full-screen raster is the
// whole physical screen, so source coordinates never read a black outside;
// at width==1920/height==1080 the filter is exactly the identity.
inline std::vector<std::byte> MakeUnifiedFullscreenLinearFixture(const std::span<const std::byte> canonical,
    const std::uint32_t width, const std::uint32_t height)
{
    if (canonical.size() != static_cast<std::size_t>(kTwoStageCanonicalWidth) * kTwoStageCanonicalHeight * 4U ||
        !IsTwoStageFullscreenSize(width, height))
    {
        throw std::runtime_error("Linear fullscreen fixture outside fixed allocation bounds");
    }
    const auto Read = [&](const int column, const int row, const std::size_t channel) -> double
    {
        const auto clampedColumn = static_cast<std::uint32_t>(std::clamp(column, 0,
            static_cast<int>(kTwoStageCanonicalWidth) - 1));
        const auto clampedRow = static_cast<std::uint32_t>(std::clamp(row, 0,
            static_cast<int>(kTwoStageCanonicalHeight) - 1));
        return std::to_integer<unsigned int>(canonical[(static_cast<std::size_t>(clampedRow) *
            kTwoStageCanonicalWidth + clampedColumn) * 4U + channel]);
    };
    std::vector<std::byte> result(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t row = 0; row < height; row++)
    {
        const double logicalY = (row + 0.5) * kTwoStageCanonicalHeight / height - 0.5;
        const int top = static_cast<int>(std::floor(logicalY));
        const double fractionY = logicalY - top;
        for (std::uint32_t column = 0; column < width; column++)
        {
            const double logicalX = (column + 0.5) * kTwoStageCanonicalWidth / width - 0.5;
            const int left = static_cast<int>(std::floor(logicalX));
            const double fractionX = logicalX - left;
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 4U;
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                const double value = Read(left, top, channel) * (1 - fractionX) * (1 - fractionY) +
                    Read(left + 1, top, channel) * fractionX * (1 - fractionY) +
                    Read(left, top + 1, channel) * (1 - fractionX) * fractionY +
                    Read(left + 1, top + 1, channel) * fractionX * fractionY;
                result[offset + channel] = static_cast<std::byte>(static_cast<unsigned int>(
                    std::clamp(std::round(value), 0.0, 255.0)));
            }
            result[offset + 3] = std::byte{255};
        }
    }
    return result;
}

// Stage-one exact box-overlap (area) upscale. Each destination pixel cell
// [column, column+1) maps back to the source interval [column*s, (column+1)*s)
// with s = 1920/width <= 1, so at most two source pixels per axis contribute
// with exact rational overlap weights. This is the tent-free area reference;
// at width==1920/height==1080 it is exactly the identity.
inline std::vector<std::byte> MakeUnifiedFullscreenAreaFixture(const std::span<const std::byte> canonical,
    const std::uint32_t width, const std::uint32_t height)
{
    if (canonical.size() != static_cast<std::size_t>(kTwoStageCanonicalWidth) * kTwoStageCanonicalHeight * 4U ||
        !IsTwoStageFullscreenSize(width, height))
    {
        throw std::runtime_error("Area fullscreen fixture outside fixed allocation bounds");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t row = 0; row < height; row++)
    {
        const double cellTop = row * kTwoStageCanonicalHeight / static_cast<double>(height);
        const double cellBottom = (row + 1) * kTwoStageCanonicalHeight / static_cast<double>(height);
        for (std::uint32_t column = 0; column < width; column++)
        {
            const double cellLeft = column * kTwoStageCanonicalWidth / static_cast<double>(width);
            const double cellRight = (column + 1) * kTwoStageCanonicalWidth / static_cast<double>(width);
            std::array<double, 3> weightedSum{};
            for (int sourceRow = static_cast<int>(std::floor(cellTop));
                sourceRow <= static_cast<int>(std::ceil(cellBottom)) - 1; sourceRow++)
            {
                if (sourceRow < 0 || sourceRow >= static_cast<int>(kTwoStageCanonicalHeight))
                {
                    continue;
                }
                const double verticalOverlap = std::min(cellBottom, sourceRow + 1.0) - std::max(cellTop, sourceRow * 1.0);
                if (verticalOverlap <= 0.0)
                {
                    continue;
                }
                for (int sourceColumn = static_cast<int>(std::floor(cellLeft));
                    sourceColumn <= static_cast<int>(std::ceil(cellRight)) - 1; sourceColumn++)
                {
                    if (sourceColumn < 0 || sourceColumn >= static_cast<int>(kTwoStageCanonicalWidth))
                    {
                        continue;
                    }
                    const double horizontalOverlap = std::min(cellRight, sourceColumn + 1.0) -
                        std::max(cellLeft, sourceColumn * 1.0);
                    if (horizontalOverlap <= 0.0)
                    {
                        continue;
                    }
                    const double weight = horizontalOverlap * verticalOverlap /
                        ((cellRight - cellLeft) * (cellBottom - cellTop));
                    const std::size_t sourceOffset = (static_cast<std::size_t>(sourceRow) *
                        kTwoStageCanonicalWidth + sourceColumn) * 4U;
                    for (std::size_t channel = 0; channel < 3; channel++)
                    {
                        weightedSum[channel] += weight *
                            std::to_integer<unsigned int>(canonical[sourceOffset + channel]);
                    }
                }
            }
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 4U;
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                result[offset + channel] = static_cast<std::byte>(static_cast<unsigned int>(
                    std::clamp(std::round(weightedSum[channel]), 0.0, 255.0)));
            }
            result[offset + 3] = std::byte{255};
        }
    }
    return result;
}

inline std::vector<std::byte> ComposeUnifiedFullscreenStage(const std::span<const std::byte> canonical,
    const std::uint32_t width, const std::uint32_t height, const UnifiedFullscreenSamplingMode mode)
{
    switch (mode)
    {
    case UnifiedFullscreenSamplingMode::Point:
        return MakeUnifiedFullscreenPointFixture(canonical, width, height);
    case UnifiedFullscreenSamplingMode::Linear:
        return MakeUnifiedFullscreenLinearFixture(canonical, width, height);
    case UnifiedFullscreenSamplingMode::Area:
        return MakeUnifiedFullscreenAreaFixture(canonical, width, height);
    default:
        throw std::runtime_error("Unknown two-stage fullscreen sampling mode");
    }
}

// Stage-two remote-view rescale: the full-screen raster shown on the remote
// desktop is viewed by the remote-control window at independent per-axis
// scales with fractional origins inside the fixed 2560x1440 captured ROI.
// Bilinear with a black outside matches both the existing single-stage
// fixture convention and the decoder's forward-sampling channel model. The
// fit check keeps every config that silently crops the canvas out of the
// comparison matrix by throwing instead.
inline std::vector<std::byte> MakeUnifiedRemoteViewResampleFixture(const std::span<const std::byte> source,
    const std::uint32_t sourceWidth, const std::uint32_t sourceHeight,
    const int horizontalScalePermille, const int verticalScalePermille,
    const int originXFractionPermille, const int originYFractionPermille)
{
    if (!IsTwoStageFullscreenSize(sourceWidth, sourceHeight) ||
        source.size() != static_cast<std::size_t>(sourceWidth) * sourceHeight * 4U ||
        horizontalScalePermille < 700 || horizontalScalePermille > 1250 ||
        verticalScalePermille < 700 || verticalScalePermille > 1250 ||
        originXFractionPermille < 0 || originXFractionPermille > 999 ||
        originYFractionPermille < 0 || originYFractionPermille > 999)
    {
        throw std::runtime_error("Remote-view resample fixture outside fixed transform bounds");
    }
    const double scaleX = horizontalScalePermille / 1000.0;
    const double scaleY = verticalScalePermille / 1000.0;
    const double originX = 47.0 + originXFractionPermille / 1000.0;
    const double originY = 31.0 + originYFractionPermille / 1000.0;
    if (originX + scaleX * sourceWidth > kTwoStageCaptureWidth ||
        originY + scaleY * sourceHeight > kTwoStageCaptureHeight)
    {
        throw std::runtime_error("Remote-view resample fixture canvas does not fit the captured ROI");
    }
    const auto Read = [&](const int column, const int row, const std::size_t channel) -> double
    {
        if (column < 0 || row < 0 || column >= static_cast<int>(sourceWidth) || row >= static_cast<int>(sourceHeight))
        {
            return 0.0;
        }
        return std::to_integer<unsigned int>(source[(static_cast<std::size_t>(row) * sourceWidth + column) * 4U + channel]);
    };
    std::vector<std::byte> result(static_cast<std::size_t>(kTwoStageCaptureWidth) * kTwoStageCaptureHeight * 4U);
    for (std::uint32_t row = 0; row < kTwoStageCaptureHeight; row++)
    {
        const double logicalY = (row + 0.5 - originY) / scaleY - 0.5;
        const int top = static_cast<int>(std::floor(logicalY));
        const double fractionY = logicalY - top;
        for (std::uint32_t column = 0; column < kTwoStageCaptureWidth; column++)
        {
            const double logicalX = (column + 0.5 - originX) / scaleX - 0.5;
            const int left = static_cast<int>(std::floor(logicalX));
            const double fractionX = logicalX - left;
            const std::size_t offset = (static_cast<std::size_t>(row) * kTwoStageCaptureWidth + column) * 4U;
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                const double value = Read(left, top, channel) * (1 - fractionX) * (1 - fractionY) +
                    Read(left + 1, top, channel) * fractionX * (1 - fractionY) +
                    Read(left, top + 1, channel) * (1 - fractionX) * fractionY +
                    Read(left + 1, top + 1, channel) * fractionX * fractionY;
                result[offset + channel] = static_cast<std::byte>(static_cast<unsigned int>(
                    std::clamp(std::round(value), 0.0, 255.0)));
            }
            result[offset + 3] = std::byte{255};
        }
    }
    return result;
}

// Deterministic full-load random frame for the gray carriers: every transport
// slot carries a near-ceiling random payload from a splitmix64 stream seeded
// per (seed, slot, index), so the rendered masks/levels exercise the complete
// symbol book instead of a few structured patterns. GrayFast payloads use the
// proven 1629-byte ceiling budget; GrayStates uses the same relative framing
// headroom below its 1350-byte information block.
struct UnifiedRandomFullLoadFrame
{
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
    std::array<std::vector<std::byte>, pbmodulation::kUnifiedMaximumFrameSlotCount> blocks{};
    std::uint32_t slotCount = 0;
    std::uint64_t sessionTagValue = 0;
    std::vector<std::byte> canonical;
};

[[nodiscard]] inline std::uint64_t AdvanceSplitMix64(std::uint64_t& state) noexcept
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
}

inline UnifiedRandomFullLoadFrame BuildUnifiedRandomFullLoadFrame(const pbprotocol::ProductVisualProfile& profile,
    const std::uint64_t seed, const std::uint64_t sequence)
{
    const bool grayFast = profile.visualProfileId == pbprotocol::kGrayFastExperimentalProfile.visualProfileId &&
        profile.visualLayoutVersion == pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion;
    const bool grayStates = profile.visualProfileId == pbprotocol::kGrayStatesExperimentalProfile.visualProfileId &&
        profile.visualLayoutVersion == pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion;
    if (!grayFast && !grayStates)
    {
        throw std::runtime_error("Random full-load frame builder only supports the gray carriers");
    }
    std::uint64_t randomState = seed;
    const std::uint64_t sessionTagValue = AdvanceSplitMix64(randomState);
    UnifiedRandomFullLoadFrame frame;
    frame.sessionTagValue = sessionTagValue;
    frame.slotCount = pbmodulation::kUnifiedGrayFrameCodewordCount;
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        profile.visualLayoutVersion, profile.visualProfileId, pbprotocol::SessionTag{sessionTagValue},
        sequence, 0x31323334U, 0};
    if (!pbprotocol::SerializeBootstrapRecord(record, frame.bootstrap))
    {
        throw std::runtime_error("Random full-load bootstrap serialization failed");
    }
    const std::uint32_t transportPayloadBytes = grayFast ? pbmodulation::kUnifiedGrayFastTransportPayloadBytes :
        pbmodulation::kUnifiedInformationBytes - 40U;
    std::array<pbmodulation::UnifiedFrameSlotInput, pbmodulation::kUnifiedMaximumFrameSlotCount> inputs{};
    for (std::uint32_t slot = 0; slot < frame.slotCount; slot++)
    {
        inputs[slot].assignment.codewordSlot = slot;
        inputs[slot].active = true;
        if (slot == 0)
        {
            inputs[slot].assignment.kind = pbmodulation::UnifiedSlotKind::Control;
            inputs[slot].assignment.controlPriority = pbmodulation::UnifiedControlPriority::SessionDescriptor;
            std::array<std::byte, 9> payload{};
            for (std::size_t index = 0; index < payload.size(); index++)
            {
                payload[index] = static_cast<std::byte>(AdvanceSplitMix64(randomState) & 0xFFU);
            }
            const pbprotocol::ControlRecordView control{pbprotocol::kControlVersion,
                pbprotocol::ControlRecordType::SessionDescriptor, static_cast<std::uint32_t>(payload.size()),
                pbprotocol::SessionTag{sessionTagValue}, payload};
            const auto controlSize = pbprotocol::GetSerializedSize(control);
            if (!controlSize)
            {
                throw std::runtime_error("Random full-load control serialization size failed");
            }
            frame.blocks[slot].resize(controlSize.Value());
            if (!pbprotocol::SerializeControlRecord(control, frame.blocks[slot]))
            {
                throw std::runtime_error("Random full-load control serialization failed");
            }
        }
        else
        {
            inputs[slot].assignment.kind = pbmodulation::UnifiedSlotKind::Transport;
            inputs[slot].assignment.controlPriority = pbmodulation::UnifiedControlPriority::NotApplicable;
            const std::size_t payloadSize = transportPayloadBytes - 4U - slot % 7U;
            std::vector<std::byte> payload(payloadSize);
            for (std::size_t index = 0; index < payload.size(); index++)
            {
                payload[index] = static_cast<std::byte>(AdvanceSplitMix64(randomState) & 0xFFU);
            }
            const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
                pbprotocol::kTransportProtocolMinor, 0, pbprotocol::SessionTag{sessionTagValue},
                700 + slot, 800 + slot, static_cast<std::uint16_t>(payload.size())};
            frame.blocks[slot].resize(pbprotocol::GetTransportSerializedSize(header));
            if (!pbprotocol::SerializeTransportBlock(header, payload, frame.blocks[slot]))
            {
                throw std::runtime_error("Random full-load transport serialization failed");
            }
        }
        inputs[slot].block = frame.blocks[slot];
    }
    frame.canonical.resize(pbmodulation::kUnifiedFrameBgraBytes);
    const auto encoded = pbmodulation::EncodeUnifiedVisualFrame(
        {frame.bootstrap, std::span<const pbmodulation::UnifiedFrameSlotInput>(inputs.data(), frame.slotCount)},
        frame.canonical);
    if (!encoded)
    {
        throw std::runtime_error("Random full-load canonical encoding failed");
    }
    return frame;
}

// Chroma-neutral bounded luma noise applied after stage two, as a test-only
// stand-in for remote-codec luma quantization. The same offset hits B/G/R of
// a pixel so gray content stays gray; the stream is deterministic per seed.
// This exists only for diagnostic comparison runs and never gates recovery.
inline void ApplyUnifiedBoundedLumaNoise(const std::span<std::byte> pixels, const std::uint32_t width,
    const std::uint32_t height, const int amplitude, const std::uint64_t seed)
{
    if (pixels.size() != static_cast<std::size_t>(width) * height * 4U || amplitude < 0 || amplitude > 32)
    {
        throw std::runtime_error("Bounded luma noise outside fixed transform bounds");
    }
    std::uint64_t state = seed;
    for (std::size_t offset = 0; offset + 3U < pixels.size(); offset += 4U)
    {
        if (amplitude == 0)
        {
            break;
        }
        const std::uint64_t value = AdvanceSplitMix64(state);
        // Map 64 random bits to a symmetric integer in [-amplitude, amplitude].
        const int noise = static_cast<int>((value % (2U * static_cast<std::uint64_t>(amplitude) + 1U)) -
            static_cast<std::int64_t>(amplitude));
        for (std::size_t channel = 0; channel < 3; channel++)
        {
            const int luma = std::to_integer<int>(pixels[offset + channel]) + noise;
            pixels[offset + channel] = static_cast<std::byte>(std::clamp(luma, 0, 255));
        }
    }
}

} // namespace pbtest
