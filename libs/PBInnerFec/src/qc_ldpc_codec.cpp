#include "pbinnerfec/qc_ldpc_codec.h"

#include "dvbs2_short_matrix.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace pbinnerfec {

namespace {

[[nodiscard]] constexpr bool GetBit(
    const std::span<const std::byte> bits,
    const std::uint32_t bitIndex) noexcept
{
    return (bits[bitIndex / 8] &
        std::byte{std::uint8_t{1u << (bitIndex % 8u)}}) != std::byte{0};
}

void SetBit(
    const std::span<std::byte> bits,
    const std::uint32_t bitIndex,
    const bool value) noexcept
{
    const std::byte mask =
        std::byte{std::uint8_t{1u << (bitIndex % 8u)}};
    if (value)
    {
        bits[bitIndex / 8] |= mask;
    }
    else
    {
        bits[bitIndex / 8] &= ~mask;
    }
}

// Bounded arithmetic note: in normal operation every message magnitude in
// this reference decoder stays on the order of the channel LLR magnitude
// (|LLR| <= 32767): the check->variable message is the minimum of the other
// variable->check magnitudes, the scale factor never amplifies (scaleNum
// <= scaleDen), and the offset only subtracts. A variable->check message
// is total(u) - message(c->u), a difference of two int32 values, computed
// in int64. In the worst case the saturating clamps keep every int32 state
// value inside int32 range, so the int64 intermediates stay below
// 3 * 2^31 * 4096 << INT64_MAX. The saturating clamps are fail-safe
// guards, not expected bounds.
constexpr std::int32_t kSaturatingInt32Min =
    std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kSaturatingInt32Max =
    std::numeric_limits<std::int32_t>::max();

std::int32_t ClampToInt32(const std::int64_t value) noexcept
{
    if (value <= kSaturatingInt32Min)
    {
        return kSaturatingInt32Min;
    }
    if (value >= kSaturatingInt32Max)
    {
        return kSaturatingInt32Max;
    }
    return static_cast<std::int32_t>(value);
}

} // namespace

namespace detail {

struct QcLdpcDecoderImpl
{
    const InnerFecProfile* profile = nullptr;
    const DvbS2ShortMatrix* matrix = nullptr;
    std::uint32_t numChecks = 0;
    // Flattened check-slot layout: slot s belongs to check
    // checkOffsets[c] <= s < checkOffsets[c+1] and carries the message from
    // variable checkVariableIds[s] to that check.
    std::vector<std::uint32_t> checkOffsets;
    std::vector<std::uint32_t> checkVariableIds;
    std::vector<std::int32_t> checkMessages;
    std::vector<std::int32_t> variableTotals;

    [[nodiscard]] std::size_t GetSlotCount() const noexcept
    {
        return checkVariableIds.size();
    }
};

void BuildCheckSlotLayout(QcLdpcDecoderImpl& impl)
{
    const DvbS2ShortMatrix& matrix = *impl.matrix;
    const std::uint32_t numChecks = matrix.parityBits;
    const std::uint32_t nBits = matrix.nBits;

    // Pass 1: count info slots per check row.
    std::vector<std::uint32_t> infoCounts(numChecks, 0);
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix.numLines; lineIndex++)
    {
        const std::uint8_t degree =
            matrix.lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix.lineShiftOffsets[lineIndex];
        for (std::uint32_t withinLine = 0;
            withinLine < matrix.mGroups; withinLine++)
        {
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint32_t row =
                    (static_cast<std::uint32_t>(
                        matrix.lineShifts[shiftOffset + shiftIndex]) +
                    withinLine * matrix.qShift) % matrix.parityBits;
                infoCounts[row]++;
            }
        }
    }

    // Slot counts per check: info slots + staircase parity slots.
    std::vector<std::uint32_t> slotCounts(numChecks, 0);
    for (std::uint32_t checkIndex = 0;
        checkIndex < numChecks; checkIndex++)
    {
        slotCounts[checkIndex] = infoCounts[checkIndex];
        slotCounts[checkIndex] += 1; // identity: parity bit checkIndex
        if (checkIndex >= 1)
        {
            slotCounts[checkIndex] += 1; // staircase: parity bit checkIndex-1
        }
    }

    const std::uint64_t slotCount =
        static_cast<std::uint64_t>(numChecks) + 1;
    impl.checkOffsets.resize(static_cast<std::size_t>(slotCount));
    impl.checkOffsets[0] = 0;
    for (std::uint32_t checkIndex = 0;
        checkIndex < numChecks; checkIndex++)
    {
        impl.checkOffsets[checkIndex + 1] =
            impl.checkOffsets[checkIndex] + slotCounts[checkIndex];
    }
    const std::uint32_t totalSlots = impl.checkOffsets[numChecks];
    impl.checkVariableIds.resize(totalSlots);
    impl.checkMessages.assign(totalSlots, 0);

    // Pass 2: fill info slots (deterministic order: line, withinLine, shift).
    std::vector<std::uint32_t> fillPosition(impl.checkOffsets.begin(),
        impl.checkOffsets.end());
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix.numLines; lineIndex++)
    {
        const std::uint8_t degree =
            matrix.lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix.lineShiftOffsets[lineIndex];
        for (std::uint32_t withinLine = 0;
            withinLine < matrix.mGroups; withinLine++)
        {
            const std::uint32_t variableIndex =
                lineIndex * matrix.mGroups + withinLine;
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint32_t row =
                    (static_cast<std::uint32_t>(
                        matrix.lineShifts[shiftOffset + shiftIndex]) +
                    withinLine * matrix.qShift) % matrix.parityBits;
                const std::uint32_t slot = fillPosition[row]++;
                impl.checkVariableIds[slot] = variableIndex;
            }
        }
    }

    // Pass 3: append staircase parity slots per check.
    for (std::uint32_t checkIndex = 0;
        checkIndex < numChecks; checkIndex++)
    {
        const std::uint32_t identityVariable =
            matrix.kBits + checkIndex;
        const std::uint32_t identitySlot = fillPosition[checkIndex]++;
        impl.checkVariableIds[identitySlot] = identityVariable;
        if (checkIndex >= 1)
        {
            const std::uint32_t staircaseVariable =
                matrix.kBits + (checkIndex - 1);
            const std::uint32_t staircaseSlot =
                fillPosition[checkIndex]++;
            impl.checkVariableIds[staircaseSlot] = staircaseVariable;
        }
    }

    impl.variableTotals.resize(nBits);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Encoder and structured syndrome
// ---------------------------------------------------------------------------

InnerFecStatus EncodeQcLdpcCodeword(
    const InnerFecProfileId profileId,
    const std::span<const std::byte> infoBits,
    const std::span<std::byte> codeword) noexcept
{
    const InnerFecProfile* profile = GetInnerFecProfile(profileId);
    if (profile == nullptr)
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::UnknownProfileId, profileId);
    }
    const DvbS2ShortMatrix* matrix =
        GetDvbS2ShortMatrix(profile->kBits);
    if (matrix == nullptr)
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::InvalidProfile, profileId);
    }
    // Registry profiles are library constants, so a validation failure can
    // only come from the live MatrixDigest gate on the embedded table.
    if (!ValidateInnerFecProfile(*profile))
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::MatrixDigestMismatch, profileId);
    }
    if (infoBits.size() != profile->GetInfoByteCount())
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::InvalidInput,
            profile->GetInfoByteCount());
    }
    if (codeword.size() != profile->GetCodewordByteCount())
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::InvalidInput,
            profile->GetCodewordByteCount());
    }
    // Enforced contract: the information prefix is copied forward with
    // memcpy semantics, so overlapping spans would be undefined behavior.
    // Any intersection, including an exact same-base overlap, is rejected
    // fail-closed.
    const auto infoBegin = infoBits.data();
    const auto infoEnd = infoBits.data() + infoBits.size();
    const auto codewordBegin = codeword.data();
    const auto codewordEnd = codeword.data() + codeword.size();
    if (infoBegin < codewordEnd && codewordBegin < infoEnd)
    {
        return InnerFecStatus::Failure(
            InnerFecErrorCode::InvalidInput, 0);
    }

    std::memcpy(
        codeword.data(), infoBits.data(), infoBits.size());
    const std::span<std::byte> parityRegion =
        codeword.subspan(infoBits.size());
    std::memset(parityRegion.data(), 0, parityRegion.size());

    // Row contributions of the information bits.
    std::array<std::uint8_t, kDvbS2ShortMaxParityBits> rowParity{};
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix->numLines; lineIndex++)
    {
        const std::uint8_t degree =
            matrix->lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix->lineShiftOffsets[lineIndex];
        for (std::uint32_t withinLine = 0;
            withinLine < matrix->mGroups; withinLine++)
        {
            const std::uint32_t infoBitIndex =
                lineIndex * matrix->mGroups + withinLine;
            if (!GetBit(infoBits, infoBitIndex))
            {
                continue;
            }
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint32_t row =
                    (static_cast<std::uint32_t>(
                        matrix->lineShifts[shiftOffset + shiftIndex]) +
                    withinLine * matrix->qShift) %
                    matrix->parityBits;
                rowParity[row] ^= 1u;
            }
        }
    }

    // Staircase chain: P[j] = A[j] XOR P[j-1] (prefix XOR), which exactly
    // cancels the identity/staircase parity connections of every row.
    for (std::uint32_t parityIndex = 1;
        parityIndex < matrix->parityBits; parityIndex++)
    {
        rowParity[parityIndex] ^= rowParity[parityIndex - 1];
    }

    for (std::uint32_t parityIndex = 0;
        parityIndex < matrix->parityBits; parityIndex++)
    {
        SetBit(
            codeword,
            profile->kBits + parityIndex,
            rowParity[parityIndex] != 0u);
    }
    return InnerFecStatus::Success();
}

InnerFecResult<bool> ComputeQcLdpcSyndrome(
    const InnerFecProfileId profileId,
    const std::span<const std::byte> codeword) noexcept
{
    const InnerFecProfile* profile = GetInnerFecProfile(profileId);
    if (profile == nullptr)
    {
        return InnerFecResult<bool>::Failure(
            InnerFecErrorCode::UnknownProfileId, profileId);
    }
    const DvbS2ShortMatrix* matrix =
        GetDvbS2ShortMatrix(profile->kBits);
    if (matrix == nullptr)
    {
        return InnerFecResult<bool>::Failure(
            InnerFecErrorCode::InvalidProfile, profileId);
    }
    // Same live MatrixDigest gate as EncodeQcLdpcCodeword and
    // QcLdpcDecoder::Create: all three public entry points fail closed on
    // embedded-table drift, so the syndrome can never evaluate against a
    // different H than the encoder/decoder use.
    if (!ValidateInnerFecProfile(*profile))
    {
        return InnerFecResult<bool>::Failure(
            InnerFecErrorCode::MatrixDigestMismatch, profileId);
    }
    if (codeword.size() != profile->GetCodewordByteCount())
    {
        return InnerFecResult<bool>::Failure(
            InnerFecErrorCode::InvalidInput,
            profile->GetCodewordByteCount());
    }

    std::array<std::uint8_t, kDvbS2ShortMaxParityBits> rowParity{};
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix->numLines; lineIndex++)
    {
        const std::uint8_t degree =
            matrix->lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix->lineShiftOffsets[lineIndex];
        for (std::uint32_t withinLine = 0;
            withinLine < matrix->mGroups; withinLine++)
        {
            const std::uint32_t infoBitIndex =
                lineIndex * matrix->mGroups + withinLine;
            if (!GetBit(codeword, infoBitIndex))
            {
                continue;
            }
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint32_t row =
                    (static_cast<std::uint32_t>(
                        matrix->lineShifts[shiftOffset + shiftIndex]) +
                    withinLine * matrix->qShift) %
                    matrix->parityBits;
                rowParity[row] ^= 1u;
            }
        }
    }

    bool syndromeZero = true;
    for (std::uint32_t checkIndex = 0;
        checkIndex < matrix->parityBits; checkIndex++)
    {
        std::uint8_t row = rowParity[checkIndex];
        // Identity connection: parity bit checkIndex.
        row ^= static_cast<std::uint8_t>(
            GetBit(codeword, profile->kBits + checkIndex) ? 1u : 0u);
        // Staircase connection: parity bit checkIndex-1 (when it exists).
        if (checkIndex >= 1)
        {
            row ^= static_cast<std::uint8_t>(
                GetBit(codeword, profile->kBits + (checkIndex - 1))
                    ? 1u
                    : 0u);
        }
        if (row != 0u)
        {
            syndromeZero = false;
            break;
        }
    }
    return InnerFecResult<bool>::Success(syndromeZero);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

QcLdpcDecoder::QcLdpcDecoder() noexcept = default;

QcLdpcDecoder::QcLdpcDecoder(QcLdpcDecoder&& other) noexcept
    : impl_(std::move(other.impl_))
{
}

QcLdpcDecoder& QcLdpcDecoder::operator=(
    QcLdpcDecoder&& other) noexcept
{
    if (this != &other)
    {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

QcLdpcDecoder::~QcLdpcDecoder() = default;

InnerFecResult<QcLdpcDecoder> QcLdpcDecoder::Create(
    const InnerFecProfileId profileId) noexcept
{
    const InnerFecProfile* profile = GetInnerFecProfile(profileId);
    if (profile == nullptr)
    {
        return InnerFecResult<QcLdpcDecoder>::Failure(
            InnerFecErrorCode::UnknownProfileId, profileId);
    }
    const DvbS2ShortMatrix* matrix =
        GetDvbS2ShortMatrix(profile->kBits);
    if (matrix == nullptr || matrix->kBits != profile->kBits)
    {
        return InnerFecResult<QcLdpcDecoder>::Failure(
            InnerFecErrorCode::InvalidProfile, profileId);
    }
    if (!ValidateInnerFecProfile(*profile))
    {
        // Registry fields are constants; a failure here means the embedded
        // table no longer matches the pinned MatrixDigest.
        return InnerFecResult<QcLdpcDecoder>::Failure(
            InnerFecErrorCode::MatrixDigestMismatch, profileId);
    }

    QcLdpcDecoder decoder;
    try
    {
        // Every allocation (the impl object and the workspace vectors) is
        // inside the try: Create is noexcept and must report OutOfMemory
        // instead of letting std::bad_alloc propagate (std::terminate).
        decoder.impl_ = std::make_unique<detail::QcLdpcDecoderImpl>();
        decoder.impl_->profile = profile;
        decoder.impl_->matrix = matrix;
        decoder.impl_->numChecks = matrix->parityBits;
        detail::BuildCheckSlotLayout(*decoder.impl_);
    }
    catch (const std::bad_alloc&)
    {
        return InnerFecResult<QcLdpcDecoder>::Failure(
            InnerFecErrorCode::OutOfMemory, 0);
    }
    return InnerFecResult<QcLdpcDecoder>::Success(std::move(decoder));
}

InnerFecResult<InnerFecDecodeOutcome> QcLdpcDecoder::Decode(
    const std::span<const std::int16_t> llr,
    const InnerFecDecodeOptions& options,
    const std::span<std::byte> outCodeword) noexcept
{
    if (impl_ == nullptr)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidState, 0);
    }
    const InnerFecProfile& profile = *impl_->profile;
    if (llr.size() != profile.nBits)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidInput, profile.nBits);
    }
    if (outCodeword.size() != profile.GetCodewordByteCount())
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidInput,
            profile.GetCodewordByteCount());
    }
    if (options.maxIterations < kQcLdpcMinIterations ||
        options.maxIterations > kQcLdpcMaxIterations)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidOption, options.maxIterations);
    }
    if (options.syndromeCheckInterval < kQcLdpcMinIterations ||
        options.syndromeCheckInterval > options.maxIterations)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidOption,
            options.syndromeCheckInterval);
    }
    if (options.offset > kQcLdpcMaxLlrOffset)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidOption, options.offset);
    }
    if (options.scaleDen < kQcLdpcMinIterations ||
        options.scaleDen > kQcLdpcMaxLlrScale)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidOption, options.scaleDen);
    }
    if (options.scaleNum > options.scaleDen)
    {
        return InnerFecResult<InnerFecDecodeOutcome>::Failure(
            InnerFecErrorCode::InvalidOption, options.scaleNum);
    }

    detail::QcLdpcDecoderImpl& impl = *impl_;
    const std::uint32_t nBits = profile.nBits;

    // Channel initialization: every variable total starts at the channel
    // LLR; every check->variable message starts at zero (the standard BP
    // "no prior evidence" initialization). A check->variable message is the
    // evidence that check carries to the variable; the variable->check
    // message is always total(v) - message(c->v) (exact invariant: both
    // sides change by the same delta whenever the stored message is
    // updated), so no separate state is needed.
    for (std::uint32_t variableIndex = 0; variableIndex < nBits;
        variableIndex++)
    {
        impl.variableTotals[variableIndex] = llr[variableIndex];
    }
    const std::size_t slotCount = impl.GetSlotCount();
    for (std::size_t slotIndex = 0; slotIndex < slotCount; slotIndex++)
    {
        impl.checkMessages[slotIndex] = 0;
    }

    const std::int64_t scaleNum = options.scaleNum;
    const std::int64_t scaleDen = options.scaleDen;
    const std::int64_t offset = options.offset;

    for (std::uint32_t iteration = 1;
        iteration <= options.maxIterations; iteration++)
    {
        for (std::uint32_t checkIndex = 0;
            checkIndex < impl.numChecks; checkIndex++)
        {
            const std::uint32_t slotBegin =
                impl.checkOffsets[checkIndex];
            const std::uint32_t slotEnd =
                impl.checkOffsets[checkIndex + 1];
            for (std::uint32_t targetSlot = slotBegin;
                targetSlot < slotEnd; targetSlot++)
            {
                // Exact min-sum over the variable->check messages of the
                // other variables of this check (order-independent):
                // magnitude = min |m_{u->c}|, sign = XOR of their signs.
                // m_{u->c} is read through the compact invariant
                // total(u) - m_{c->u}; this value is unchanged by this
                // check's own in-row updates (total and stored message move
                // by the same delta), so the layered in-row slot order is
                // well defined. Every check has degree >= 2 (staircase
                // parity structure), so the minimum is over a non-empty set.
                std::int64_t magnitude = 0;
                bool hasMagnitude = false;
                bool positiveSign = true;
                for (std::uint32_t otherSlot = slotBegin;
                    otherSlot < slotEnd; otherSlot++)
                {
                    if (otherSlot == targetSlot)
                    {
                        continue;
                    }
                    const std::uint32_t otherVariable =
                        impl.checkVariableIds[otherSlot];
                    const std::int64_t variableToCheck =
                        static_cast<std::int64_t>(
                            impl.variableTotals[otherVariable]) -
                        impl.checkMessages[otherSlot];
                    if (variableToCheck < 0)
                    {
                        positiveSign = !positiveSign;
                    }
                    const std::int64_t absMessage =
                        variableToCheck < 0
                            ? -variableToCheck
                            : variableToCheck;
                    if (!hasMagnitude || absMessage < magnitude)
                    {
                        magnitude = absMessage;
                        hasMagnitude = true;
                    }
                }
                // Fixed-point scale (never amplifying: scaleNum <= scaleDen)
                // and the offset min-sum magnitude reduction.
                magnitude = magnitude * scaleNum / scaleDen;
                std::int32_t newMessage = 0;
                if (magnitude > offset)
                {
                    const std::int32_t magnitudeAfterOffset =
                        ClampToInt32(magnitude - offset);
                    newMessage = positiveSign
                        ? magnitudeAfterOffset
                        : -magnitudeAfterOffset;
                }
                const std::int32_t oldMessage =
                    impl.checkMessages[targetSlot];
                impl.checkMessages[targetSlot] = newMessage;
                const std::uint32_t variableIndex =
                    impl.checkVariableIds[targetSlot];
                impl.variableTotals[variableIndex] =
                    ClampToInt32(static_cast<std::int64_t>(
                        impl.variableTotals[variableIndex]) +
                        newMessage - oldMessage);
            }
        }

        if (iteration % options.syndromeCheckInterval != 0)
        {
            continue;
        }

        // Hard-decision syndrome over the current variable totals: each
        // check slot list is exactly the row's incident variables.
        bool syndromeZero = true;
        for (std::uint32_t checkIndex = 0;
            checkIndex < impl.numChecks; checkIndex++)
        {
            const std::uint32_t slotBegin =
                impl.checkOffsets[checkIndex];
            const std::uint32_t slotEnd =
                impl.checkOffsets[checkIndex + 1];
            std::uint32_t parity = 0;
            for (std::uint32_t slotIndex = slotBegin;
                slotIndex < slotEnd; slotIndex++)
            {
                const std::int32_t total = impl.variableTotals[
                    impl.checkVariableIds[slotIndex]];
                parity ^= static_cast<std::uint32_t>(total < 0);
            }
            if (parity != 0u)
            {
                syndromeZero = false;
                break;
            }
        }
        if (!syndromeZero)
        {
            continue;
        }

        std::memset(outCodeword.data(), 0, outCodeword.size());
        for (std::uint32_t variableIndex = 0;
            variableIndex < nBits; variableIndex++)
        {
            if (impl.variableTotals[variableIndex] < 0)
            {
                outCodeword[variableIndex / 8] |=
                    std::byte{std::uint8_t{1u << (variableIndex % 8u)}};
            }
        }
        return InnerFecResult<InnerFecDecodeOutcome>::Success(
            InnerFecDecodeOutcome{iteration, iteration});
    }

    // Fail closed: no output is published on failure.
    return InnerFecResult<InnerFecDecodeOutcome>::Failure(
        InnerFecErrorCode::SyndromeFailure,
        options.maxIterations);
}

const InnerFecProfile* QcLdpcDecoder::GetProfile() const noexcept
{
    return impl_ == nullptr ? nullptr : impl_->profile;
}

} // namespace pbinnerfec
