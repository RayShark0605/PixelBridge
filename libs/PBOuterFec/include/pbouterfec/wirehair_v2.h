#pragma once

#include "pbouterfec/outer_fec_decoder_resource.h"
#include "pbouterfec/outer_fec_result.h"
#include "pbprotocol/descriptor_binding.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace pbouterfec {

inline constexpr std::uint64_t kWirehairV2CertifiedProfileId =
    0x4B295BBB47F4F9C9ULL;
inline constexpr std::uint64_t kWirehairV2MixedProfileId =
    0xE161CE5D456F9BB7ULL;
inline constexpr std::uint64_t kWirehairV2MixedMix2ProfileId =
    0x20A4F27A870612A2ULL;
inline constexpr std::uint32_t kWirehairV2MinimumBlockCount = 2;
inline constexpr std::uint32_t kWirehairV2MaximumBlockCount = 64000;

namespace detail {
struct WirehairV2DecoderImplementation;
}

namespace test {
class DecoderTestAccess;
}

// Source-compatible name retained for the first Wirehair wrapper callers.
// Both names refer to the same receiver-wide, cross-mode admission manager.
using WirehairV2DecoderResourceManager = OuterFecDecoderResourceManager;

// Owns one canonical serialized-profile Wirehair V2 encoder. The serialized
// profile identifies equation compatibility; it is not sender authentication.
// A single instance has one owner and must not be called concurrently.
class WirehairV2Encoder
{
public:
    WirehairV2Encoder(const WirehairV2Encoder&) = delete;
    WirehairV2Encoder& operator=(const WirehairV2Encoder&) = delete;
    WirehairV2Encoder(WirehairV2Encoder&& other) noexcept;
    WirehairV2Encoder& operator=(WirehairV2Encoder&& other) noexcept;
    ~WirehairV2Encoder();

    [[nodiscard]] static OuterFecResult<WirehairV2Encoder> Create(
        std::span<const std::byte> encodedSegment,
        std::uint32_t outerBlockBytes,
        std::uint64_t profileId = kWirehairV2CertifiedProfileId);

    // Carousel recreation accepts only the exact saved canonical descriptor
    // and bytes whose BLAKE3-256 equals SegmentDescriptor::encodedDigest.
    [[nodiscard]] static OuterFecResult<WirehairV2Encoder> Recreate(
        std::span<const std::byte> exactEncodedSegment,
        const pbprotocol::SegmentDescriptor& segmentDescriptor);

    // outerBlockId is forwarded unchanged as Wirehair's blockId. Systematic
    // IDs are [0, K); IDs >= K are repair equations.
    [[nodiscard]] OuterFecResult<std::uint32_t> EncodeBlock(
        std::uint32_t outerBlockId,
        std::span<std::byte> output);

    // Deliberately return the fixed 32-byte owner by value so callers persist
    // a snapshot rather than a reference tied to this codec's lifetime.
    // cppcheck-suppress returnByReference
    [[nodiscard]] pbprotocol::WirehairV2SerializedProfile GetSerializedProfile() const noexcept;
    [[nodiscard]] std::uint32_t GetBlockCount() const noexcept;
    [[nodiscard]] std::uint64_t GetEncodedSize() const noexcept;
    [[nodiscard]] std::uint32_t GetOuterBlockBytes() const noexcept;

private:
    WirehairV2Encoder() noexcept = default;
    void Release() noexcept;

    void* codecHandle_ = nullptr;
    const void* backend_ = nullptr;
    pbprotocol::WirehairV2SerializedProfile serializedProfile_{};
    std::uint64_t encodedSize_ = 0;
    std::uint32_t outerBlockBytes_ = 0;
    std::uint32_t blockCount_ = 0;
    bool moved_ = false;
    std::optional<OuterFecError> terminalError_;
};

// Owns one bounded Wirehair V2 decoder. Create() revalidates canonical profile
// binding and performs receiver-wide admission through the required shared
// resource manager before allocating wrapper state or a third-party codec.
// Recover() verifies encoded integrity, but an in-band digest does not
// authenticate the sender or replace higher-layer raw/final-file verification.
// A single instance must not be called concurrently.
class WirehairV2Decoder
{
public:
    WirehairV2Decoder(const WirehairV2Decoder&) = delete;
    WirehairV2Decoder& operator=(const WirehairV2Decoder&) = delete;
    WirehairV2Decoder(WirehairV2Decoder&& other) noexcept;
    WirehairV2Decoder& operator=(WirehairV2Decoder&& other) noexcept;
    ~WirehairV2Decoder();

    [[nodiscard]] static OuterFecResult<WirehairV2Decoder> Create(
        const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
        std::uint32_t expectedOuterBlockBytes,
        const OuterFecDecoderResourceManager& resourceManager);

    [[nodiscard]] OuterFecResult<DecodeDisposition> DecodeBlock(
        std::uint32_t outerBlockId,
        std::span<const std::byte> payload);

    // Recovery is not successful until the exact recovered bytes match the
    // EncodedDigest carried by the bound descriptor. A mismatch latches the
    // decoder terminally; third-party codec success alone is insufficient.
    [[nodiscard]] OuterFecResult<std::uint64_t> Recover(
        std::span<std::byte> output);

    // The canonical serialized profile the decoder was bound to at Create
    // time, for cross-checking replay sources that persist their own profile
    // snapshot (resume.state cache records, design doc section 31.2). A
    // validated bound profile is never zero-filled, so a zero-filled result
    // identifies an empty (moved-from) decoder.
    [[nodiscard]] pbprotocol::WirehairV2SerializedProfile GetBoundSerializedProfile() const noexcept;
    // Telemetry/admission introspection only. This is the number of distinct
    // equation IDs retained in the bounded fingerprint table and returns zero
    // for an empty decoder.
    [[nodiscard]] std::uint64_t GetAcceptedBlockCount() const noexcept;

private:
    friend class test::DecoderTestAccess;

    WirehairV2Decoder() noexcept = default;
    void Release() noexcept;

    [[nodiscard]] static OuterFecResult<WirehairV2Decoder>
    CreateFromDescriptor(
        const pbprotocol::SegmentDescriptor& segmentDescriptor,
        const OuterFecDecoderResourceManager& resourceManager);

    std::unique_ptr<detail::WirehairV2DecoderImplementation> implementation_;
};

} // namespace pbouterfec
