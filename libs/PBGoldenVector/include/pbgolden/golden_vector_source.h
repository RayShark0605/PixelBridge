#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbgolden {

// ---------------------------------------------------------------------------
// PB-GoldenVector-1: deterministic golden vector byte source.
//
// Every committed golden vector under tests/golden/ is produced by exactly
// one function of this library. The functions are pure byte sources:
// deterministic (SplitMix64 with fixed seeds, no CSPRNG and no clock),
// free of process state, and allocation only for the returned vector.
//
// The source is independent of the golden *digest* pins: the registry
// (golden_vector_registry.h) stores the frozen BLAKE3-256 values, while the
// source reproduces the bytes. The harness (PBGoldenVectorCheck) proves the
// two agree; a disagreement is a regression, not a resync opportunity.
// ---------------------------------------------------------------------------

// Deterministic PRNG (splitmix64). Identical formula to the PBInnerFec and
// PBModulation test helpers so shared patterns stay byte-compatible.
class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) noexcept;

    [[nodiscard]] std::uint64_t Next() noexcept;

    // Consumes count outputs without returning them (deterministic stream
    // positioning for the LDPC golden bit stretches).
    void Skip(const std::uint32_t count) noexcept
    {
        for (std::uint32_t index = 0; index < count; index++)
        {
            const std::uint64_t discarded = Next();
            (void)discarded;
        }
    }

private:
    std::uint64_t state_;
};

// Little-endian u64 seed built from the first 8 ASCII bytes of tag. The
// tags are self-documenting ("PB-TX-C0" = PB golden Transport block 0).
[[nodiscard]] inline constexpr std::uint64_t MakeTagSeed(
    const char tag[8]) noexcept
{
    std::uint64_t value = 0;
    for (int byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned char>(tag[byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

// Frozen golden session tag (the PB-Bootstrap-1 golden record tag field,
// little-endian bytes 16..24 of the 44-byte record).
inline constexpr std::uint64_t kGoldenSessionTag = 0x81DF204BD997BAD0ULL;

// SplitMix64 seed for the LDPC golden information patterns. Same seed as
// the existing PBInnerFec/PBModulation golden convention.
inline constexpr std::uint64_t kLdpcPatternSeed = 0xC0FFEEULL;

// SplitMix64 seeds for the golden Transport block payloads.
inline constexpr std::uint64_t kTransportCanonicalPayloadSeed =
    MakeTagSeed("PB-TX-C0");
inline constexpr std::uint64_t kTransportSecondPayloadSeed =
    MakeTagSeed("PB-TX-C1");
inline constexpr std::uint64_t kTransportMaxPayloadSeed =
    MakeTagSeed("PB-TX-M1");

// SplitMix64 seed for the interleave golden region pattern.
inline constexpr std::uint64_t kInterleaveRegionSeed =
    MakeTagSeed("PB-INTLV");

// The G1-Transport information block is sized exactly to the DVB-S2 Short
// Robust profile information size (10800 bits = 1350 bytes), so one
// 2025-byte codeword carries exactly one Transport block with no internal
// zero padding.
inline constexpr std::size_t kG1TransportInfoBlockBytes = 1350;
inline constexpr std::size_t kG1TransportPayloadBytes =
    kG1TransportInfoBlockBytes - 36; // 1314

// ---------------------------------------------------------------------------
// protocol category vectors (tests/golden/protocol/)
// ---------------------------------------------------------------------------

// 44-byte frozen PB-Bootstrap-1 golden record (byte-identical to the
// existing kBootstrapGolden pins in tests/PBProtocol and
// tests/PBModulation).
[[nodiscard]] std::vector<std::byte> GenerateBootstrapRecord();

// 67-byte frozen PB-Control-1 SessionDescriptor record (golden type 1
// record, 37-byte payload).
[[nodiscard]] std::vector<std::byte> GenerateControlSessionDescriptor();

// 30-byte minimal PB-Control-1 record: type 1, zero sequence and tag,
// empty payload.
[[nodiscard]] std::vector<std::byte> GenerateControlEmpty();

// 65,536-byte maximum PB-Control-1 record: type 1, zero sequence and tag,
// 65,506-byte zero payload.
[[nodiscard]] std::vector<std::byte> GenerateControlMaximum();

// One of the three fragments of the 67-byte golden control record
// (fragmentIndex in {0, 1, 2}; 48/48/43 bytes).
[[nodiscard]] std::vector<std::byte> GenerateControlFragment(
    const std::uint16_t fragmentIndex);

// 37-byte SessionDescriptor payload (file size 117, segment count 1),
// byte-identical to the existing PBProtocol and PBModulation Golden arrays.
[[nodiscard]] std::vector<std::byte> GenerateSessionDescriptorPayload();

// 110-byte DirectRepeat SegmentDescriptor payload (ordinal 0, offset 0,
// raw/encoded size 117, outerBlockBytes 16, 0x20..0x3f digests).
[[nodiscard]] std::vector<std::byte> GenerateDirectRepeatSegmentPayload();

// 142-byte WirehairV2 SegmentDescriptor payload (ordinal 0, offset 0,
// raw size 200, encoded size 117, Zstandard, 16-byte blocks, canonical
// 117x16 profile).
[[nodiscard]] std::vector<std::byte> GenerateWirehairSegmentPayload();

// 65-byte FinalManifest payload (digest start byte 0xA0).
[[nodiscard]] std::vector<std::byte> GenerateFinalManifestPayload();

// 32-byte canonical Wirehair V2 serialized profile (117 x 16 certified,
// seed attempt 0). Hand-built byte layout (PBGoldenVector intentionally
// does not link the third-party Wirehair baseline).
[[nodiscard]] std::vector<std::byte> GenerateWirehairCanonicalDescriptor();

// 36-byte minimal Transport block (golden layout, empty payload).
[[nodiscard]] std::vector<std::byte> GenerateTransportBlockMinimum();

// 1,350-byte canonical Transport block: golden tag, ordinal 0, block 0,
// 1,314-byte SplitMix64(kTransportCanonicalPayloadSeed) payload.
[[nodiscard]] std::vector<std::byte> GenerateTransportBlockCanonical();

// 65,571-byte maximum Transport block: golden tag, ordinal 1, block
// 0xFFFFFFFF, 65,535-byte SplitMix64(kTransportMaxPayloadSeed) payload.
[[nodiscard]] std::vector<std::byte> GenerateTransportBlockMaxPayload();

// ---------------------------------------------------------------------------
// ldpc category vectors (tests/golden/ldpc/)
// ---------------------------------------------------------------------------
// Each 2,025-byte codeword independently starts at bit zero of
// SplitMix64(kLdpcPatternSeed), matching the existing PBInnerFec Golden
// convention. The profile-specific information lengths are 10800, 11880
// and 13320 bits, LSB-first packed.
[[nodiscard]] std::vector<std::byte> GenerateLdpcCodewordRobust();
[[nodiscard]] std::vector<std::byte> GenerateLdpcCodewordBalanced();
[[nodiscard]] std::vector<std::byte> GenerateLdpcCodewordFast();

// ---------------------------------------------------------------------------
// interleave category vectors (tests/golden/interleave/)
// ---------------------------------------------------------------------------

// 8,192-byte mapping sample: for each of the 16 phases and logical tile
// 0..63, one (u32LE logical tile, u32LE physical tile) pair, phase order
// outermost.
[[nodiscard]] std::vector<std::byte> GenerateInterleaveMappingSample();

// 56,168-byte deterministic logical region pattern
// (SplitMix64(kInterleaveRegionSeed) byte stream).
[[nodiscard]] std::vector<std::byte> GenerateInterleaveRegionLogical();

// ApplyInterleave(regionLogical, frameSequence = 7).
[[nodiscard]] std::vector<std::byte>
    GenerateInterleaveRegionPhysicalPhase7();

// ---------------------------------------------------------------------------
// raster category vectors (tests/golden/raster/)
// ---------------------------------------------------------------------------

// 275-byte canonical PBVM reference region manifest.
[[nodiscard]] std::vector<std::byte> GenerateReferenceRasterManifest();

// 1,350-byte canonical Transport block (== GenerateTransportBlockCanonical).
[[nodiscard]] std::vector<std::byte> GenerateG1TransportInfoBlock();

// 1,350-byte decoded Transport block: the reference perfect-LLR decode
// chain (frame encode -> demod -> syndrome -> bounded LLR decode with
// +/-100 int16 samples -> info-block extraction -> strict Transport parse)
// applied to the G1-Transport frame. On any chain failure throws a typed
// exception that RecomputeGoldenVector converts to RecomputeFailed; it never
// substitutes an empty vector for expected bytes.
[[nodiscard]] std::vector<std::byte> GenerateG1TransportDecoded();

// 2,700-byte two-codeword information block vector: the canonical block
// (ordinal 0) followed by the second block (ordinal 1, block 0, 1,314-byte
// SplitMix64(kTransportSecondPayloadSeed) payload).
[[nodiscard]] std::vector<std::byte> GenerateG1Transport2CwInfoBlock();

// ---------------------------------------------------------------------------
// Frozen reference frame payloads (8.3 MB frames are not committed to the
// repository; only their digests are pinned)
// ---------------------------------------------------------------------------

struct GoldenFramePayload
{
    // Exactly 44 bytes: the frozen PB-Bootstrap-1 record.
    std::array<std::byte, 44> bootstrap{};
    // Exactly 240 bytes: the per-frame control window.
    std::array<std::byte, 240> control{};
    // Exactly 56,168 bytes: the data region payload.
    std::vector<std::byte> data;
};

// G0: all-zero payload.
[[nodiscard]] GoldenFramePayload MakeZeroFramePayload();
// G1: canonical payload (golden bootstrap, 67-byte golden control record
// zero-padded to 240, 2025-byte Robust golden codeword zero-padded to the
// data region). Byte-identical to the existing G1 construction in
// tests/PBModulation.
[[nodiscard]] GoldenFramePayload MakeCanonicalFramePayload();
// G2: all-0xFF payload.
[[nodiscard]] GoldenFramePayload MakeMaxFramePayload();
// G1-Transport: G1 bootstrap/control, data region = one 2025-byte Robust
// codeword over the 1,350-byte canonical Transport information block,
// canonical zero padding of the unused tail.
[[nodiscard]] GoldenFramePayload MakeG1TransportFramePayload();
// G1-Transport-2CW: G1 bootstrap/control, data region = two 2025-byte
// Robust codewords (canonical block ordinal 0, second block ordinal 1),
// canonical zero padding of the unused tail.
[[nodiscard]] GoldenFramePayload MakeG1Transport2CwFramePayload();

// Test/tooling-only independent PB-ReferenceRaster-1 oracle. This constructs
// all 8,294,400 BGRA bytes directly from the frozen literal geometry, Gray
// mapping and lane/tile order; it does not call EncodeReferenceFrame or any
// PBModulation private raster helper. It throws std::invalid_argument when the
// data payload does not have the frozen 56,168-byte size.
[[nodiscard]] std::vector<std::byte> GenerateReferenceRasterOracle(
    const GoldenFramePayload& payload);

struct FrameDigestResult
{
    bool success = false;
    // Diagnostic on failure (encoder/error code and offset).
    std::string detail;
    // BLAKE3-256 of the PBRW raw container bytes (primary pin).
    std::array<std::byte, 32> rawBlake3{};
    // BLAKE3-256 of the full PNG stream bytes (secondary pin, bound to the
    // pinned libpng 1.6.58 baseline).
    std::array<std::byte, 32> pngBlake3{};
    // Production PBRW bytes versus the independently constructed PBRW
    // oracle. The offset includes the 28-byte PBRW header.
    bool rawOracleMatches = false;
    std::size_t rawExpectedSize = 0;
    std::size_t rawActualSize = 0;
    std::size_t rawFirstDifferingOffset = 0;
    std::size_t rawDifferingBytes = 0;
    std::byte rawExpectedByte{};
    std::byte rawActualByte{};
    bool rawExpectedEof = false;
    bool rawActualEof = false;
    // Production PNG decoded back to BGRA versus the independent raster
    // oracle. This distinguishes pixel drift from a compression-stream-only
    // digest drift.
    bool pngPixelsMatch = false;
    std::size_t pngPixelFirstDifferingOffset = 0;
    std::size_t pngPixelDifferingBytes = 0;
    std::byte pngPixelExpectedByte{};
    std::byte pngPixelActualByte{};
};

// Renders the full 1920x1080 frame from payload (8,294,400 BGRA bytes in
// memory) and computes both container digests. Never aborts: failures are
// reported through success/detail.
[[nodiscard]] FrameDigestResult ComputeFrameDigests(
    const GoldenFramePayload& payload);

// BLAKE3-256 of the 275-byte canonical reference region manifest.
[[nodiscard]] std::array<std::byte, 32> ComputeManifestDigest();

} // namespace pbgolden
