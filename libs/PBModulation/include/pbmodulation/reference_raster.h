#pragma once

#include "pbmodulation/modulation_result.h"
#include "pbmodulation/reference_visual_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbmodulation {

// One canonical reference frame payload (design 16.3): every frame is a
// complete raster rebuilt from this payload plus the fixed regions.
struct ReferenceFrameInput
{
    // Exactly 44 bytes: the frozen PB-Bootstrap-1 record. Opaque to the
    // raster layer.
    std::array<std::byte, kReferenceBootstrapRecordBytes> bootstrapRecord{};
    // Exactly 240 bytes: the per-frame control window (the raster layer is
    // content-agnostic; the control plane owns record framing).
    std::span<const std::byte> controlWindow;
    // Exactly 56168 bytes: the data region payload, e.g. Inner-FEC
    // codewords followed by canonical zero padding of the unused tail.
    std::span<const std::byte> data;
};

struct DecodedReferenceFrame
{
    std::array<std::byte, kReferenceBootstrapRecordBytes> bootstrapRecord{};
    std::array<std::byte, kReferenceControlWindowBytes> controlWindow{};
    // kReferenceDataRegionBytes bytes on success.
    std::vector<std::byte> data;

    bool operator==(const DecodedReferenceFrame&) const = default;
};

// Renders the full canonical 1920x1080 BGRA raster into outBgra, which must
// be exactly kReferenceFrameBgraBytes. The raster is deterministic: the
// same input always produces the same bytes (no timestamps, no allocator-
// dependent content, no persistent frame state).
[[nodiscard]] ModulationStatus EncodeReferenceFrame(
    const ReferenceFrameInput& input,
    std::span<std::byte> outBgra) noexcept;

// Demodulates a full 1920x1080 BGRA raster. Fail-closed (design 4.3): any
// violation (off-constellation level, ambiguous level, chroma/alpha
// contract, torn bootstrap, frozen-region mismatch) rejects the whole frame
// and produces no output; the error offset is the canvas linear pixel index
// (y * 1920 + x) of the first violating pixel.
[[nodiscard]] ModulationResult<DecodedReferenceFrame> DecodeReferenceFrame(
    std::span<const std::byte> bgra) noexcept;

// Non-allocating form of DecodeReferenceFrame. The out spans must have the
// frozen sizes (44 / 240 / 56168); on failure none of them is modified.
[[nodiscard]] ModulationStatus DecodeReferenceFrameInto(
    std::span<const std::byte> bgra,
    std::span<std::byte> outBootstrap,
    std::span<std::byte> outControl,
    std::span<std::byte> outData) noexcept;

} // namespace pbmodulation