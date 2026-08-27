#pragma once

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/reference_raster.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace phase0gate
{

// A reference-only composition of already frozen contracts, not a new wire
// envelope or a Certified Profile. Existing opaque raster Golden inputs are
// not implicitly declared valid packets of this stricter composition.
struct ReferenceBinding
{
    std::uint64_t visualProfileId = 0x5042524546524153ULL;
    std::uint8_t layoutVersion = 1;
    std::uint64_t innerProfileId = pbinnerfec::kInnerFecProfileIdRobust;
    std::uint32_t nBits = 16200;
    std::uint32_t kBits = 10800;
    std::uint64_t matrixId = pbinnerfec::kInnerFecMatrixIdRobust;
    std::array<std::byte, 32> matrixDigest{
        std::byte{0xc6}, std::byte{0xd8}, std::byte{0xea}, std::byte{0xbe}, std::byte{0x59}, std::byte{0xd8}, std::byte{0xbc}, std::byte{0x85},
        std::byte{0xb2}, std::byte{0x07}, std::byte{0x67}, std::byte{0x66}, std::byte{0xd7}, std::byte{0x45}, std::byte{0xbc}, std::byte{0xa6},
        std::byte{0x18}, std::byte{0x2f}, std::byte{0x16}, std::byte{0xe0}, std::byte{0x19}, std::byte{0x11}, std::byte{0x76}, std::byte{0x53},
        std::byte{0x12}, std::byte{0x1f}, std::byte{0x1e}, std::byte{0x24}, std::byte{0x7b}, std::byte{0xe3}, std::byte{0x4d}, std::byte{0xd3}};
    std::uint8_t bitOrder = 1;
    std::uint8_t puncturing = 0;
    std::uint8_t interleave = 0;
    std::uint32_t featureFlags = 0;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t outerBlockBytes = 1314;
    std::uint32_t controlWindowBytes = 240;
    std::uint32_t dataRegionBytes = 56168;

    bool operator==(const ReferenceBinding&) const = default;
};

inline constexpr ReferenceBinding kReferenceBinding{};
inline constexpr std::size_t kReferenceInfoBytes = 1350;
inline constexpr std::size_t kReferenceCodewordBytes = 2025;
inline constexpr std::size_t kReferenceCodewordsPerFrame = 27;

enum class ReferenceFrameErrorCode
{
    BindingMismatch,
    RasterInvalid,
    BootstrapInvalid,
    UnsupportedProfile,
    UnsupportedLayout,
    ControlLength,
    ControlInvalid,
    NonCanonicalPadding,
    SyndromeFailure,
    InnerDecodeFailure,
    TransportInvalid,
    SessionTagMismatch
};

class ReferenceFrameFailure final : public std::runtime_error
{
public:
    ReferenceFrameFailure(ReferenceFrameErrorCode errorCode, std::size_t errorOffset);
    [[nodiscard]] ReferenceFrameErrorCode GetCode() const noexcept;
    [[nodiscard]] std::size_t GetOffset() const noexcept;

private:
    ReferenceFrameErrorCode errorCode_;
    std::size_t errorOffset_;
};

struct ReferenceDecodedBlock
{
    pbprotocol::TransportBlockHeader header{};
    std::array<std::byte, 1314> paddedPayload{};
};

struct ReferenceDecodedFrame
{
    pbprotocol::BootstrapRecord bootstrap{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> control{};
    std::size_t controlBytes = 0;
    std::array<ReferenceDecodedBlock, kReferenceCodewordsPerFrame> blocks{};
    std::size_t blockCount = 0;
};

// Decoding is independent of sender state and has no admission side effects.
// Every frame is validated before callers can feed its bytes to ReceiverIngress.
class ReferenceFrameDecoder
{
public:
    explicit ReferenceFrameDecoder(const ReferenceBinding& binding = kReferenceBinding);
    [[nodiscard]] ReferenceDecodedFrame Decode(std::span<const std::byte> frameBgra);
    [[nodiscard]] std::size_t GetWorkingBytes() const noexcept;

private:
    ReferenceBinding binding_;
    pbinnerfec::QcLdpcDecoder decoder_;
    pbinnerfec::InnerFecDecodeOptions decodeOptions_{};
    std::vector<std::byte> data_;
    std::vector<std::int16_t> llr_;
    std::array<std::byte, kReferenceCodewordBytes> decodedCodeword_{};
};

} // namespace phase0gate
