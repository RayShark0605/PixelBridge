#include "pbinnerfec/inner_fec_profile.h"

#include "dvbs2_short_matrix.h"
#include "pbprotocol/blake3_digest.h"

namespace pbinnerfec {

namespace {

// Pinned MatrixDigest golden vectors: BLAKE3-256 of the canonical
// serialization (see inner_fec_profile.h) of each embedded table. They were
// derived independently from the verified upstream data and are also pinned
// as hex literals in the test suite.
constexpr std::array<std::byte, 32> kPinnedMatrixDigestRobust = {
    std::byte{0xC6}, std::byte{0xD8}, std::byte{0xEA}, std::byte{0xBE}, std::byte{0x59}, std::byte{0xD8}, std::byte{0xBC}, std::byte{0x85},
    std::byte{0xB2}, std::byte{0x07}, std::byte{0x67}, std::byte{0x66}, std::byte{0xD7}, std::byte{0x45}, std::byte{0xBC}, std::byte{0xA6},
    std::byte{0x18}, std::byte{0x2F}, std::byte{0x16}, std::byte{0xE0}, std::byte{0x19}, std::byte{0x11}, std::byte{0x76}, std::byte{0x53},
    std::byte{0x12}, std::byte{0x1F}, std::byte{0x1E}, std::byte{0x24}, std::byte{0x7B}, std::byte{0xE3}, std::byte{0x4D}, std::byte{0xD3}
};

constexpr std::array<std::byte, 32> kPinnedMatrixDigestBalanced = {
    std::byte{0x2A}, std::byte{0x49}, std::byte{0x19}, std::byte{0x86}, std::byte{0xB3}, std::byte{0x12}, std::byte{0x15}, std::byte{0x8C},
    std::byte{0x16}, std::byte{0xBC}, std::byte{0xDF}, std::byte{0x54}, std::byte{0x88}, std::byte{0x7A}, std::byte{0xCF}, std::byte{0x28},
    std::byte{0x72}, std::byte{0xE5}, std::byte{0x8D}, std::byte{0x5A}, std::byte{0x28}, std::byte{0x88}, std::byte{0xA4}, std::byte{0x1B},
    std::byte{0xE3}, std::byte{0x8B}, std::byte{0xF0}, std::byte{0x5C}, std::byte{0xF5}, std::byte{0x34}, std::byte{0x2D}, std::byte{0xA3}
};

constexpr std::array<std::byte, 32> kPinnedMatrixDigestFast = {
    std::byte{0x72}, std::byte{0x09}, std::byte{0x93}, std::byte{0xE2}, std::byte{0x4A}, std::byte{0xE1}, std::byte{0x18}, std::byte{0xD5},
    std::byte{0x69}, std::byte{0x89}, std::byte{0x99}, std::byte{0x57}, std::byte{0xC0}, std::byte{0xA8}, std::byte{0x25}, std::byte{0x7E},
    std::byte{0x97}, std::byte{0xA6}, std::byte{0x95}, std::byte{0x01}, std::byte{0x58}, std::byte{0xA2}, std::byte{0xA0}, std::byte{0xD4},
    std::byte{0x19}, std::byte{0x03}, std::byte{0xE4}, std::byte{0x20}, std::byte{0x6E}, std::byte{0x77}, std::byte{0xE7}, std::byte{0x6C}
};

const InnerFecProfile kRobustProfile = {
    kInnerFecProfileIdRobust,
    16200,
    10800,
    5400,
    360,
    15,
    30,
    kInnerFecMatrixIdRobust,
    kPinnedMatrixDigestRobust,
    static_cast<std::uint8_t>(SystematicBitOrder::DvbS2ShortNatural),
    static_cast<std::uint8_t>(PuncturingRule::None),
    static_cast<std::uint8_t>(ParityStructure::StaircaseDual)};

const InnerFecProfile kBalancedProfile = {
    kInnerFecProfileIdBalanced,
    16200,
    11880,
    4320,
    360,
    12,
    33,
    kInnerFecMatrixIdBalanced,
    kPinnedMatrixDigestBalanced,
    static_cast<std::uint8_t>(SystematicBitOrder::DvbS2ShortNatural),
    static_cast<std::uint8_t>(PuncturingRule::None),
    static_cast<std::uint8_t>(ParityStructure::StaircaseDual)};

const InnerFecProfile kFastProfile = {
    kInnerFecProfileIdFast,
    16200,
    13320,
    2880,
    360,
    8,
    37,
    kInnerFecMatrixIdFast,
    kPinnedMatrixDigestFast,
    static_cast<std::uint8_t>(SystematicBitOrder::DvbS2ShortNatural),
    static_cast<std::uint8_t>(PuncturingRule::None),
    static_cast<std::uint8_t>(ParityStructure::StaircaseDual)};

void AppendUint32LittleEndian(
    const std::uint32_t value,
    const std::span<std::byte> target,
    const std::size_t offset) noexcept
{
    target[offset] = static_cast<std::byte>(value & 0xFFu);
    target[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    target[offset + 2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    target[offset + 3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

} // namespace

const InnerFecProfile* GetInnerFecProfile(
    const InnerFecProfileId profileId) noexcept
{
    switch (profileId)
    {
        case kInnerFecProfileIdRobust:
            return &kRobustProfile;
        case kInnerFecProfileIdBalanced:
            return &kBalancedProfile;
        case kInnerFecProfileIdFast:
            return &kFastProfile;
        default:
            return nullptr;
    }
}

const InnerFecProfile* GetInnerFecProfileByMatrixId(
    const InnerFecMatrixId matrixId) noexcept
{
    switch (matrixId)
    {
        case kInnerFecMatrixIdRobust:
            return &kRobustProfile;
        case kInnerFecMatrixIdBalanced:
            return &kBalancedProfile;
        case kInnerFecMatrixIdFast:
            return &kFastProfile;
        default:
            return nullptr;
    }
}

bool ValidateInnerFecProfile(const InnerFecProfile& profile) noexcept
{
    if (profile.nBits != kDvbS2ShortFrameNBits)
    {
        return false;
    }
    if (profile.kBits != 10800 && profile.kBits != 11880 &&
        profile.kBits != 13320)
    {
        return false;
    }
    if (profile.parityBits != profile.nBits - profile.kBits)
    {
        return false;
    }
    if (profile.mGroups != kDvbS2ShortFrameMGroups)
    {
        return false;
    }
    // Checked relation: K must be an exact multiple of the group count.
    if (profile.kBits % profile.mGroups != 0)
    {
        return false;
    }
    const std::uint32_t expectedLines = profile.kBits / profile.mGroups;
    if (profile.numLines != expectedLines)
    {
        return false;
    }
    // Checked relation: (N-K) must be an exact multiple of the group count.
    if (profile.parityBits % profile.mGroups != 0)
    {
        return false;
    }
    if (profile.qShift != profile.parityBits / profile.mGroups)
    {
        return false;
    }
    if (profile.systematicBitOrder !=
        static_cast<std::uint8_t>(SystematicBitOrder::DvbS2ShortNatural))
    {
        return false;
    }
    if (profile.puncturingRule !=
        static_cast<std::uint8_t>(PuncturingRule::None))
    {
        return false;
    }
    if (profile.parityStructure !=
        static_cast<std::uint8_t>(ParityStructure::StaircaseDual))
    {
        return false;
    }
    switch (profile.kBits)
    {
        case 10800:
            if (profile.profileId != kInnerFecProfileIdRobust ||
                profile.matrixId != kInnerFecMatrixIdRobust)
            {
                return false;
            }
            break;
        case 11880:
            if (profile.profileId != kInnerFecProfileIdBalanced ||
                profile.matrixId != kInnerFecMatrixIdBalanced)
            {
                return false;
            }
            break;
        case 13320:
            if (profile.profileId != kInnerFecProfileIdFast ||
                profile.matrixId != kInnerFecMatrixIdFast)
            {
                return false;
            }
            break;
        default:
            return false;
    }
    // Live gate: the embedded table must still digest to the pinned value,
    // and the profile's own matrixDigest field must agree with that digest
    // (the field is the identity-bound digest of the canonical
    // serialization; a mismatching field fails closed).
    const std::array<std::byte, 32> computedDigest =
        ComputeInnerFecMatrixDigest(profile);
    if (profile.matrixDigest != computedDigest)
    {
        return false;
    }
    switch (profile.kBits)
    {
        case 10800:
            return computedDigest == kPinnedMatrixDigestRobust;
        case 11880:
            return computedDigest == kPinnedMatrixDigestBalanced;
        case 13320:
            return computedDigest == kPinnedMatrixDigestFast;
        default:
            return false;
    }
}

std::span<const std::byte> SerializeInnerFecMatrixInto(
    const InnerFecProfile& profile,
    const std::span<std::byte> out) noexcept
{
    const DvbS2ShortMatrix* matrix =
        GetDvbS2ShortMatrix(profile.kBits);
    if (matrix == nullptr)
    {
        return {};
    }
    if (out.size() < kMaxInnerFecMatrixSerializationBytes)
    {
        return {};
    }
    std::size_t offset = 0;
    out[offset++] = std::byte{'P'};
    out[offset++] = std::byte{'B'};
    out[offset++] = std::byte{'M'};
    out[offset++] = std::byte{'X'};
    out[offset++] = std::byte{1}; // version
    out[offset++] = std::byte{profile.systematicBitOrder};
    out[offset++] = std::byte{profile.puncturingRule};
    out[offset++] = std::byte{profile.parityStructure};
    AppendUint32LittleEndian(profile.nBits, out, offset);
    offset += 4;
    AppendUint32LittleEndian(profile.kBits, out, offset);
    offset += 4;
    AppendUint32LittleEndian(profile.parityBits, out, offset);
    offset += 4;
    AppendUint32LittleEndian(profile.mGroups, out, offset);
    offset += 4;
    AppendUint32LittleEndian(profile.qShift, out, offset);
    offset += 4;
    AppendUint32LittleEndian(profile.numLines, out, offset);
    offset += 4;
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix->numLines; lineIndex++)
    {
        const std::uint8_t degree =
            matrix->lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix->lineShiftOffsets[lineIndex];
        out[offset++] = std::byte{degree};
        for (std::uint32_t shiftIndex = 0;
            shiftIndex < degree; shiftIndex++)
        {
            const std::uint16_t shiftValue =
                matrix->lineShifts[shiftOffset + shiftIndex];
            out[offset++] =
                static_cast<std::byte>(shiftValue & 0xFFu);
            out[offset++] =
                static_cast<std::byte>((shiftValue >> 8) & 0xFFu);
        }
    }
    return out.first(offset);
}

std::vector<std::byte> SerializeInnerFecMatrix(
    const InnerFecProfile& profile) noexcept
{
    std::vector<std::byte> buffer(kMaxInnerFecMatrixSerializationBytes);
    const std::span<const std::byte> serialized =
        SerializeInnerFecMatrixInto(profile, buffer);
    if (serialized.empty())
    {
        return {};
    }
    buffer.resize(serialized.size());
    return buffer;
}

std::array<std::byte, 32> ComputeInnerFecMatrixDigest(
    const InnerFecProfile& profile) noexcept
{
    std::array<std::byte, kMaxInnerFecMatrixSerializationBytes> buffer{};
    const std::span<const std::byte> serialized =
        SerializeInnerFecMatrixInto(profile, buffer);
    if (serialized.empty())
    {
        return {};
    }
    return pbprotocol::ComputeBlake3Digest(serialized);
}

} // namespace pbinnerfec
