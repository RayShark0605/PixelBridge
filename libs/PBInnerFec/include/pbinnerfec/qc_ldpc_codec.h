#pragma once

#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/inner_fec_result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pbinnerfec {

namespace detail {
struct QcLdpcDecoderImpl;
}

// ---------------------------------------------------------------------------
// Deterministic DVB-S2 Short QC-LDPC encoder.
//
// Systematic: the first GetInfoByteCount() bytes of the codeword are the
// information bytes unchanged. Parity is computed with the standard DVB-S2
// recursive construction: per-row contributions from the embedded matrix
// lines, followed by the staircase prefix-XOR chain. The function is
// stateless, allocation-free, and deterministic: identical inputs always
// produce identical codewords.
//
// Before encoding, the embedded matrix table is digested and compared with
// the profile-pinned MatrixDigest; a mismatch fails closed with
// MatrixDigestMismatch.
//
// Bit packing is LSB first within each byte (see SystematicBitOrder).
// infoBits and codeword must not overlap: the information prefix is
// copied forward into the codeword buffer (memcpy semantics).
// ---------------------------------------------------------------------------
[[nodiscard]] InnerFecStatus EncodeQcLdpcCodeword(
    const InnerFecProfileId profileId,
    const std::span<const std::byte> infoBits,
    const std::span<std::byte> codeword) noexcept;

// ---------------------------------------------------------------------------
// Structured syndrome check H * c (mod 2) == 0 over the full embedded
// matrix (information lines + staircase parity structure). Returns true when
// the packed codeword is a valid codeword.
// ---------------------------------------------------------------------------
[[nodiscard]] InnerFecResult<bool> ComputeQcLdpcSyndrome(
    const InnerFecProfileId profileId,
    const std::span<const std::byte> codeword) noexcept;

// ---------------------------------------------------------------------------
// LLR convention (explicit, frozen for this reference):
//   llr[i] > 0  -> bit i is more likely to be 0
//   llr[i] < 0  -> bit i is more likely to be 1
//   hard decision: bit = (llr[i] < 0) ? 1 : 0, so a zero LLR decides 0.
// The convention is a receiver-local reference contract, not a wire field.
// ---------------------------------------------------------------------------

struct InnerFecDecodeOptions
{
    // In [1, 100].
    std::uint32_t maxIterations = 48;
    // In [1, maxIterations]. After every syndromeCheckInterval-th full layer
    // pass the hard decisions are syndrome-checked; a zero syndrome stops
    // decoding early. The first check happens after the first full pass.
    std::uint32_t syndromeCheckInterval = 1;
    // Offset min-sum magnitude offset, in LLR units, in [0, 32767].
    std::uint32_t offset = 2048;
    // Fixed-point min-sum scale num/den with 1 <= den <= 4096 and
    // 0 <= num <= den (no amplification). num = den is the exact min-sum
    // magnitude; (3, 4) selects normalized min-sum.
    std::uint32_t scaleNum = 1;
    std::uint32_t scaleDen = 1;
};

struct InnerFecDecodeOutcome
{
    // Full layer passes executed before the zero syndrome was observed.
    std::uint32_t iterationsUsed = 0;
    // Iteration at which the syndrome passed (== iterationsUsed on success).
    std::uint32_t syndromePassedIteration = 0;

    bool operator==(const InnerFecDecodeOutcome&) const = default;
};

// Bounds used by option validation.
inline constexpr std::uint32_t kQcLdpcMinIterations = 1;
inline constexpr std::uint32_t kQcLdpcMaxIterations = 100;
inline constexpr std::uint32_t kQcLdpcMaxLlrOffset = 32767;
inline constexpr std::uint32_t kQcLdpcMaxLlrScale = 4096;

// Reference soft decoder: layered offset min-sum over the embedded DVB-S2
// Short matrix, processing check rows 0..(N-K-1) in order. Messages use
// saturating int32 arithmetic; a single instance owns one fixed workspace
// (worst case well under 1 MiB, allocated once at Create, never per
// iteration). A single instance has one owner and must not be called
// concurrently.
//
// Success semantics: the hard decisions form a valid codeword (zero
// syndrome). Success does NOT prove the information bits are the sender's:
// a low-confidence channel can converge to a different valid codeword. The
// transport CRC32C of the information bytes remains the caller's final gate
// (design document section 14.3).
//
// Failure semantics: SyndromeFailure after maxIterations without a zero
// syndrome; detail carries iterationsUsed. On failure the output span is
// left untouched.
class QcLdpcDecoder
{
public:
    // The default constructor is declared (not defaulted) here so that it
    // is defined out-of-line in the .cpp, where QcLdpcDecoderImpl is
    // complete; the unique_ptr member must never be destroyed against an
    // incomplete type.
    QcLdpcDecoder() noexcept;
    QcLdpcDecoder(const QcLdpcDecoder&) = delete;
    QcLdpcDecoder& operator=(const QcLdpcDecoder&) = delete;
    QcLdpcDecoder(QcLdpcDecoder&& other) noexcept;
    QcLdpcDecoder& operator=(QcLdpcDecoder&& other) noexcept;
    ~QcLdpcDecoder();

    // Validates the profile (including the pinned MatrixDigest gate) and
    // expands the fixed edge workspace. Fails closed on unknown profile,
    // digest mismatch, or workspace allocation failure.
    [[nodiscard]] static InnerFecResult<QcLdpcDecoder> Create(
        const InnerFecProfileId profileId) noexcept;

    // llr must have exactly nBits (16200) int16 samples; outCodeword must
    // have exactly nBits/8 (2025) bytes. On success the hard-decision
    // codeword is packed into outCodeword in the canonical systematic bit
    // order.
    [[nodiscard]] InnerFecResult<InnerFecDecodeOutcome> Decode(
        const std::span<const std::int16_t> llr,
        const InnerFecDecodeOptions& options,
        const std::span<std::byte> outCodeword) noexcept;

    // Stable pointer to the frozen profile, or nullptr when this instance
    // is default-constructed or moved-from.
    [[nodiscard]] const InnerFecProfile* GetProfile() const noexcept;

private:
    std::unique_ptr<detail::QcLdpcDecoderImpl> impl_;
};

} // namespace pbinnerfec
