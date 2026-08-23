#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kMaximumAcceptedFuzzInputBytes =
    pbprotocol::kMaximumControlFragmentBytes + 1;
constexpr std::size_t kMaximumRandomInputBytes = 512;
constexpr std::size_t kMaximumSequenceOperations = 8;
constexpr std::uint64_t kDefaultIterations = 100000;
constexpr std::uint64_t kDefaultSeed = 0x50424354524C0001ULL;

constexpr std::array<std::byte, pbprotocol::kBootstrapRecordBytes>
    kBootstrapSeed{
        std::byte{0x50}, std::byte{0x42}, std::byte{0x52}, std::byte{0x47},
        std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0xD0}, std::byte{0xBA}, std::byte{0x97}, std::byte{0xD9},
        std::byte{0x4B}, std::byte{0x20}, std::byte{0xDF}, std::byte{0x81},
        std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
        std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
        std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xEA}, std::byte{0xE1}, std::byte{0x88}, std::byte{0xD4}};

constexpr std::array<std::byte, 67> kControlSeed{
    std::byte{0x50}, std::byte{0x42}, std::byte{0x43}, std::byte{0x52},
    std::byte{0x01}, std::byte{0x01},
    std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
    std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    std::byte{0xD0}, std::byte{0xBA}, std::byte{0x97}, std::byte{0xD9},
    std::byte{0x4B}, std::byte{0x20}, std::byte{0xDF}, std::byte{0x81},
    std::byte{0x43}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
    std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
    std::byte{0x75}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01},
    std::byte{0xC8}, std::byte{0x83}, std::byte{0x38}, std::byte{0xA1}};

[[nodiscard]] std::uint8_t GetByte(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.empty())
    {
        return 0;
    }
    return std::to_integer<std::uint8_t>(input[offset % input.size()]);
}

[[nodiscard]] std::uint16_t ReadUint16(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        GetByte(input, offset) |
        static_cast<std::uint16_t>(GetByte(input, offset + 1U)) << 8U);
}

void StoreUint16(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint16_t value) noexcept
{
    if (offset > output.size() || sizeof(value) > output.size() - offset)
    {
        std::abort();
    }
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        output[offset + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
}

void StoreUint64(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint64_t value) noexcept
{
    if (offset > output.size() || sizeof(value) > output.size() - offset)
    {
        std::abort();
    }
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        output[offset + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
}

void StoreUint32(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint32_t value) noexcept
{
    if (offset > output.size() || sizeof(value) > output.size() - offset)
    {
        std::abort();
    }
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        output[offset + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
}

void RefreshCrc(
    const std::span<std::byte> bytes,
    const std::size_t crcBytes) noexcept
{
    if (bytes.size() < crcBytes)
    {
        std::abort();
    }
    const std::size_t crcOffset = bytes.size() - crcBytes;
    StoreUint32(
        bytes,
        crcOffset,
        pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(bytes).first(crcOffset)));
}

[[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes>
MakeStructuredBootstrap(const std::span<const std::byte> input) noexcept
{
    auto bytes = kBootstrapSeed;
    for (std::size_t byteIndex = 4; byteIndex < 40; byteIndex++)
    {
        bytes[byteIndex] ^= static_cast<std::byte>(
            GetByte(input, byteIndex - 3U));
    }
    RefreshCrc(bytes, 4);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeStructuredControl(
    const std::span<const std::byte> input)
{
    const std::size_t payloadBytes = input.empty()
        ? 0
        : std::min(
            input.size() - 1U,
            pbprotocol::kMaximumControlPayloadBytes);
    const std::size_t recordBytes =
        pbprotocol::kMinimumControlRecordBytes + payloadBytes;
    std::vector<std::byte> bytes(recordBytes);
    std::copy(
        pbprotocol::kControlRecordMagic.begin(),
        pbprotocol::kControlRecordMagic.end(),
        bytes.begin());
    bytes[4] = static_cast<std::byte>(GetByte(input, 1));
    bytes[5] = static_cast<std::byte>(GetByte(input, 2));
    for (std::size_t byteIndex = 6; byteIndex < 22; byteIndex++)
    {
        bytes[byteIndex] = static_cast<std::byte>(GetByte(input, byteIndex));
    }
    StoreUint32(
        bytes,
        22,
        static_cast<std::uint32_t>(recordBytes));
    for (std::size_t payloadIndex = 0;
         payloadIndex < payloadBytes;
         payloadIndex++)
    {
        bytes[pbprotocol::kControlRecordPrefixBytes + payloadIndex] =
            static_cast<std::byte>(GetByte(input, payloadIndex + 3U));
    }
    RefreshCrc(bytes, pbprotocol::kControlRecordCrcBytes);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeStructuredFragment(
    const std::span<const std::byte> input)
{
    const std::size_t payloadBytes = 30U +
        static_cast<std::size_t>(GetByte(input, 1) % 227U);
    std::uint16_t fragmentCount = static_cast<std::uint16_t>(
        1U + GetByte(input, 2) % 4U);
    std::uint16_t fragmentIndex = static_cast<std::uint16_t>(
        GetByte(input, 3) % fragmentCount);
    std::uint32_t totalRecordBytes = fragmentCount == 1
        ? static_cast<std::uint32_t>(payloadBytes)
        : static_cast<std::uint32_t>(payloadBytes + fragmentCount - 1U);

    switch (GetByte(input, 4) % 5U)
    {
    case 1:
        fragmentCount = 0;
        break;
    case 2:
        fragmentIndex = fragmentCount;
        break;
    case 3:
        totalRecordBytes = 29;
        break;
    case 4:
        fragmentCount = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(
                totalRecordBytes + 1U,
                std::numeric_limits<std::uint16_t>::max()));
        break;
    default:
        break;
    }

    std::vector<std::byte> bytes(
        pbprotocol::kControlFragmentPrefixBytes + payloadBytes +
        pbprotocol::kControlFragmentCrcBytes);
    for (std::size_t byteIndex = 0; byteIndex < sizeof(std::uint64_t); byteIndex++)
    {
        bytes[byteIndex] = static_cast<std::byte>(GetByte(input, byteIndex + 5U));
    }
    StoreUint16(bytes, 8, fragmentIndex);
    StoreUint16(bytes, 10, fragmentCount);
    StoreUint32(bytes, 12, totalRecordBytes);
    StoreUint16(bytes, 16, static_cast<std::uint16_t>(payloadBytes));
    StoreUint16(bytes, 18, ReadUint16(input, 13));
    for (std::size_t payloadIndex = 0;
         payloadIndex < payloadBytes;
         payloadIndex++)
    {
        bytes[pbprotocol::kControlFragmentPrefixBytes + payloadIndex] =
            static_cast<std::byte>(GetByte(input, payloadIndex + 15U));
    }
    RefreshCrc(bytes, pbprotocol::kControlFragmentCrcBytes);
    return bytes;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kMinimumControlFragmentBytes>
MakeMetadataFragment(
    const std::uint64_t controlRecordId,
    const std::uint16_t fragmentIndex,
    const std::uint16_t fragmentCount,
    const std::uint32_t totalRecordBytes,
    const std::byte payloadByte) noexcept
{
    std::array<std::byte, pbprotocol::kMinimumControlFragmentBytes> bytes{};
    StoreUint64(bytes, 0, controlRecordId);
    StoreUint16(bytes, 8, fragmentIndex);
    StoreUint16(bytes, 10, fragmentCount);
    StoreUint32(bytes, 12, totalRecordBytes);
    StoreUint16(bytes, 16, 1);
    StoreUint16(bytes, 18, 0);
    bytes[pbprotocol::kControlFragmentPrefixBytes] = payloadByte;
    RefreshCrc(bytes, pbprotocol::kControlFragmentCrcBytes);
    return bytes;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kMinimumControlFragmentBytes>
MakeStructuredExtremeFragment(const std::span<const std::byte> input) noexcept
{
    constexpr std::array<std::uint16_t, 8> fragmentCounts{
        0,
        1,
        2,
        30,
        4096,
        static_cast<std::uint16_t>(UINT16_MAX - 1U),
        UINT16_MAX,
        UINT16_MAX};
    constexpr std::array<std::uint32_t, 8> totalRecordByteCounts{
        0,
        29,
        30,
        31,
        65535,
        65536,
        65537,
        UINT32_MAX};

    const std::uint16_t fragmentCount = fragmentCounts[
        GetByte(input, 1) % fragmentCounts.size()];
    const std::uint32_t totalRecordBytes = totalRecordByteCounts[
        GetByte(input, 2) % totalRecordByteCounts.size()];
    std::uint16_t fragmentIndex = 0;
    switch (GetByte(input, 3) % 4U)
    {
    case 1:
        fragmentIndex = fragmentCount == 0
            ? 0
            : static_cast<std::uint16_t>(fragmentCount - 1U);
        break;
    case 2:
        fragmentIndex = fragmentCount;
        break;
    case 3:
        fragmentIndex = UINT16_MAX;
        break;
    default:
        break;
    }

    return MakeMetadataFragment(
        0xE000000000000000ULL | GetByte(input, 4),
        fragmentIndex,
        fragmentCount,
        totalRecordBytes,
        static_cast<std::byte>(GetByte(input, 5)));
}

[[nodiscard]] std::vector<std::vector<std::byte>> MakeRecordFragments(
    const std::span<const std::byte> recordBytes,
    const std::uint64_t recordId,
    const std::uint16_t fragmentPayloadBytes = 24)
{
    const auto countResult = pbprotocol::GetControlFragmentCount(
        recordBytes,
        fragmentPayloadBytes);
    if (!countResult)
    {
        std::abort();
    }

    std::vector<std::vector<std::byte>> fragments;
    fragments.reserve(countResult.Value());
    for (std::uint16_t fragmentIndex = 0;
         fragmentIndex < countResult.Value();
         fragmentIndex++)
    {
        const auto fragmentResult = pbprotocol::GetControlFragment(
            recordId,
            recordBytes,
            fragmentIndex,
            fragmentPayloadBytes);
        if (!fragmentResult)
        {
            std::abort();
        }
        const auto sizeResult = pbprotocol::GetSerializedSize(
            fragmentResult.Value());
        if (!sizeResult)
        {
            std::abort();
        }
        std::vector<std::byte> bytes(sizeResult.Value());
        if (!pbprotocol::SerializeControlFragment(fragmentResult.Value(), bytes))
        {
            std::abort();
        }
        fragments.push_back(std::move(bytes));
    }
    return fragments;
}

[[nodiscard]] std::vector<std::vector<std::byte>> MakeSeedFragments(
    const std::uint64_t recordId)
{
    return MakeRecordFragments(kControlSeed, recordId);
}

[[nodiscard]] std::vector<std::byte> MakeConflictingSessionControlRecord()
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const auto controlRecordResult = pbprotocol::ParseControlRecord(
        kControlSeed);
    if (!controlRecordResult)
    {
        std::abort();
    }
    const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
        controlRecordResult.Value().payload,
        resourcePolicy);
    if (!descriptorResult)
    {
        std::abort();
    }

    pbprotocol::SessionDescriptor changedDescriptor = descriptorResult.Value();
    changedDescriptor.originalFileSize++;
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    if (!pbprotocol::SerializeSessionDescriptor(
            changedDescriptor,
            resourcePolicy,
            payload))
    {
        std::abort();
    }

    const pbprotocol::ControlRecordView changedRecord{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        controlRecordResult.Value().controlSequence + 1U,
        controlRecordResult.Value().sessionTag,
        payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(changedRecord);
    if (!sizeResult)
    {
        std::abort();
    }
    std::vector<std::byte> recordBytes(sizeResult.Value());
    if (!pbprotocol::SerializeControlRecord(changedRecord, recordBytes))
    {
        std::abort();
    }
    return recordBytes;
}

void ExerciseCanonicalParsers(const std::span<const std::byte> input)
{
    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(input);
    if (bootstrapResult)
    {
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> reserialized{};
        if (!pbprotocol::SerializeBootstrapRecord(
                bootstrapResult.Value(),
                reserialized) ||
            !std::ranges::equal(reserialized, input))
        {
            std::abort();
        }
    }

    const auto controlResult = pbprotocol::ParseControlRecord(input);
    if (controlResult)
    {
        std::vector<std::byte> reserialized(input.size());
        if (!pbprotocol::SerializeControlRecord(
                controlResult.Value(),
                reserialized) ||
            !std::ranges::equal(reserialized, input))
        {
            std::abort();
        }

        auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
            pbprotocol::GetDefaultReceiverResourcePolicy());
        if (!receiverResult)
        {
            std::abort();
        }
        auto receiver = std::move(receiverResult).Value();
        (void)receiver.ReceiveControlRecord(input);
    }

    const auto fragmentResult = pbprotocol::ParseControlFragment(input);
    if (fragmentResult)
    {
        std::vector<std::byte> reserialized(input.size());
        if (!pbprotocol::SerializeControlFragment(
                fragmentResult.Value(),
                reserialized) ||
            !std::ranges::equal(reserialized, input))
        {
            std::abort();
        }

        auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
            pbprotocol::GetDefaultReceiverResourcePolicy());
        if (!receiverResult)
        {
            std::abort();
        }
        auto receiver = std::move(receiverResult).Value();
        (void)receiver.ReceiveControlFragment(input, 0);
    }
}

[[nodiscard]] bool ExerciseStructuredExtremeMetadata(
    const std::span<const std::byte> input)
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxControlFragmentsPerRecord = UINT16_MAX;
    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        resourcePolicy);
    if (!receiverResult)
    {
        return false;
    }
    auto receiver = std::move(receiverResult).Value();
    const std::size_t baselineReassemblyBytes =
        receiver.ControlReassemblyBytesInUse();

    const auto maximumCountFragment = MakeMetadataFragment(
        0xE001ULL,
        static_cast<std::uint16_t>(UINT16_MAX - 1U),
        UINT16_MAX,
        static_cast<std::uint32_t>(pbprotocol::kMaximumControlRecordBytes),
        std::byte{0xA5});
    const auto maximumCountParseResult = pbprotocol::ParseControlFragment(
        maximumCountFragment);
    if (!maximumCountParseResult ||
        maximumCountParseResult.Value().fragmentCount != UINT16_MAX ||
        maximumCountParseResult.Value().totalRecordBytes !=
            pbprotocol::kMaximumControlRecordBytes)
    {
        return false;
    }
    const auto maximumCountReceiveResult = receiver.ReceiveControlFragment(
        maximumCountFragment,
        1);
    if (!maximumCountReceiveResult ||
        maximumCountReceiveResult.Value().disposition !=
            pbprotocol::ControlFragmentReceiveDisposition::Stored ||
        receiver.ActiveControlReassemblyCount() != 1 ||
        receiver.ControlReassemblyBytesInUse() <= baselineReassemblyBytes ||
        receiver.ControlReassemblyBytesInUse() >
            resourcePolicy.maxControlReassemblyBytes)
    {
        return false;
    }

    const auto maximumTotalFragment = MakeMetadataFragment(
        0xE002ULL,
        1,
        2,
        static_cast<std::uint32_t>(pbprotocol::kMaximumControlRecordBytes),
        std::byte{0x5A});
    const auto maximumTotalReceiveResult = receiver.ReceiveControlFragment(
        maximumTotalFragment,
        2);
    if (!maximumTotalReceiveResult ||
        maximumTotalReceiveResult.Value().disposition !=
            pbprotocol::ControlFragmentReceiveDisposition::Stored ||
        receiver.ActiveControlReassemblyCount() != 2 ||
        receiver.ControlReassemblyBytesInUse() >
            resourcePolicy.maxControlReassemblyBytes)
    {
        return false;
    }

    const auto oversizedTotalFragment = MakeMetadataFragment(
        0xE003ULL,
        0,
        1,
        UINT32_MAX,
        std::byte{0x3C});
    const auto oversizedTotalParseResult = pbprotocol::ParseControlFragment(
        oversizedTotalFragment);
    const auto oversizedTotalReceiveResult = receiver.ReceiveControlFragment(
        oversizedTotalFragment,
        3);
    if (oversizedTotalParseResult || oversizedTotalReceiveResult ||
        oversizedTotalParseResult.Error().code !=
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded ||
        oversizedTotalReceiveResult.Error().code !=
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded ||
        receiver.ActiveControlReassemblyCount() != 2)
    {
        return false;
    }

    const auto impossibleCountFragment = MakeMetadataFragment(
        0xE004ULL,
        0,
        UINT16_MAX,
        static_cast<std::uint32_t>(pbprotocol::kMinimumControlRecordBytes),
        std::byte{0xC3});
    const auto impossibleCountParseResult = pbprotocol::ParseControlFragment(
        impossibleCountFragment);
    const auto impossibleCountReceiveResult = receiver.ReceiveControlFragment(
        impossibleCountFragment,
        4);
    if (impossibleCountParseResult || impossibleCountReceiveResult ||
        impossibleCountParseResult.Error().code !=
            pbprotocol::ProtocolErrorCode::InvalidControlFragment ||
        impossibleCountReceiveResult.Error().code !=
            pbprotocol::ProtocolErrorCode::InvalidControlFragment ||
        receiver.ActiveControlReassemblyCount() != 2)
    {
        return false;
    }

    receiver.ResetControlReassembly();
    if (receiver.ActiveControlReassemblyCount() != 0 ||
        receiver.ControlReassemblyBytesInUse() != baselineReassemblyBytes)
    {
        return false;
    }

    const auto structuredExtremeFragment = MakeStructuredExtremeFragment(input);
    ExerciseCanonicalParsers(structuredExtremeFragment);
    return true;
}

void ExerciseStructuredReassembly(const std::span<const std::byte> input)
{
    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!receiverResult)
    {
        std::abort();
    }
    auto receiver = std::move(receiverResult).Value();
    const auto fragments = MakeSeedFragments(0x0102030405060708ULL);

    const std::size_t operationCount = input.size() <= 1
        ? fragments.size()
        : std::min(input.size() - 1U, kMaximumSequenceOperations);
    for (std::size_t operationIndex = 0;
         operationIndex < operationCount;
         operationIndex++)
    {
        const std::uint8_t operation = input.size() <= 1
            ? static_cast<std::uint8_t>(operationIndex)
            : GetByte(input, operationIndex + 1U);
        const std::size_t fragmentIndex =
            static_cast<std::size_t>(operation & 0x03U) % fragments.size();
        const std::span<const std::byte> selectedFragment =
            fragments[fragmentIndex];

        if ((operation & 0x80U) == 0)
        {
            (void)receiver.ReceiveControlFragment(
                selectedFragment,
                operationIndex);
            continue;
        }

        const auto parsedResult = pbprotocol::ParseControlFragment(
            selectedFragment);
        if (!parsedResult)
        {
            std::abort();
        }
        std::vector<std::byte> payload(
            parsedResult.Value().payload.begin(),
            parsedResult.Value().payload.end());
        payload[0] ^= std::byte{0x01};
        pbprotocol::ControlFragmentView conflictingFragment =
            parsedResult.Value();
        conflictingFragment.payload = payload;
        std::vector<std::byte> conflictingBytes(selectedFragment.size());
        if (!pbprotocol::SerializeControlFragment(
                conflictingFragment,
                conflictingBytes))
        {
            std::abort();
        }
        (void)receiver.ReceiveControlFragment(
            conflictingBytes,
            operationIndex);
    }
}

void ExerciseStructuredResourceSequence()
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxConcurrentControlReassemblies = 1;
    resourcePolicy.maxControlReassemblyInactivityObservations = 2;
    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        resourcePolicy);
    if (!receiverResult)
    {
        std::abort();
    }
    auto receiver = std::move(receiverResult).Value();
    const auto firstFragments = MakeSeedFragments(1);
    const auto secondFragments = MakeSeedFragments(2);
    (void)receiver.ReceiveControlFragment(firstFragments.front(), 10);
    (void)receiver.ReceiveControlFragment(secondFragments.front(), 10);
    (void)receiver.AdvanceObservationOrdinal(12);
    (void)receiver.AdvanceObservationOrdinal(13);
    receiver.ResetControlReassembly();
}

[[nodiscard]] bool ExerciseStructuredDescriptorConflict()
{
    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!receiverResult)
    {
        return false;
    }
    auto receiver = std::move(receiverResult).Value();
    const auto firstFragments = MakeRecordFragments(kControlSeed, 0xD001ULL);
    const std::vector<std::byte> changedRecord =
        MakeConflictingSessionControlRecord();
    const auto changedFragments = MakeRecordFragments(
        changedRecord,
        0xD002ULL);

    std::uint64_t observationOrdinal = 1;
    bool firstInserted = false;
    for (std::size_t fragmentIndex = 0;
         fragmentIndex < firstFragments.size();
         fragmentIndex++)
    {
        const auto result = receiver.ReceiveControlFragment(
            firstFragments[fragmentIndex],
            observationOrdinal);
        observationOrdinal++;
        if (!result)
        {
            return false;
        }
        if (fragmentIndex + 1U == firstFragments.size())
        {
            firstInserted = result.Value().disposition ==
                pbprotocol::ControlFragmentReceiveDisposition::
                    DescriptorInserted;
        }
    }

    bool descriptorConflict = false;
    for (std::size_t fragmentIndex = 0;
         fragmentIndex < changedFragments.size();
         fragmentIndex++)
    {
        const auto result = receiver.ReceiveControlFragment(
            changedFragments[fragmentIndex],
            observationOrdinal);
        observationOrdinal++;
        if (fragmentIndex + 1U == changedFragments.size())
        {
            descriptorConflict = !result &&
                result.Error().code ==
                    pbprotocol::ProtocolErrorCode::DescriptorConflict;
        }
        else if (!result)
        {
            return false;
        }
    }

    const auto blockedResult = receiver.ReceiveControlRecord(kControlSeed);
    return firstInserted && descriptorConflict && !blockedResult &&
        blockedResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict &&
        receiver.ActiveSessionCount() == 1;
}

void ExerciseInput(const std::span<const std::byte> input)
{
    if (input.size() > kMaximumAcceptedFuzzInputBytes)
    {
        return;
    }

    ExerciseCanonicalParsers(input);
    if (input.empty())
    {
        return;
    }

    switch (GetByte(input, 0) % 7U)
    {
    case 0:
    {
        const auto structuredBootstrap = MakeStructuredBootstrap(input);
        ExerciseCanonicalParsers(structuredBootstrap);
        break;
    }
    case 1:
    {
        const std::vector<std::byte> structuredControl =
            MakeStructuredControl(input);
        ExerciseCanonicalParsers(structuredControl);
        break;
    }
    case 2:
    {
        const std::vector<std::byte> structuredFragment =
            MakeStructuredFragment(input);
        ExerciseCanonicalParsers(structuredFragment);
        break;
    }
    case 3:
        ExerciseStructuredReassembly(input);
        break;
    case 4:
        ExerciseStructuredResourceSequence();
        break;
    case 5:
        if (!ExerciseStructuredDescriptorConflict())
        {
            std::abort();
        }
        break;
    default:
        if (!ExerciseStructuredExtremeMetadata(input))
        {
            std::abort();
        }
        break;
    }
}

[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

[[nodiscard]] bool ParseUint64(
    const std::string_view text,
    std::uint64_t& value) noexcept
{
    const auto parseResult = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size();
}

#if !defined(PB_USE_LIBFUZZER)

[[nodiscard]] bool RequireSelfTest(
    const bool condition,
    const std::string_view description)
{
    if (!condition)
    {
        std::cerr << "STRUCTURED_SELF_TEST_FAILED case=" << description << '\n';
    }
    return condition;
}

int RunStructuredSelfTest()
{
    auto invalidBootstrapVersion = kBootstrapSeed;
    invalidBootstrapVersion[4] = std::byte{0x02};
    RefreshCrc(invalidBootstrapVersion, 4);
    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(
        invalidBootstrapVersion);
    if (!RequireSelfTest(
            !bootstrapResult &&
                bootstrapResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::UnsupportedBootstrapVersion,
            "bootstrap-version-after-crc"))
    {
        return 1;
    }

    auto invalidControlType = kControlSeed;
    invalidControlType[5] = std::byte{0xFF};
    RefreshCrc(invalidControlType, pbprotocol::kControlRecordCrcBytes);
    const auto controlTypeResult = pbprotocol::ParseControlRecord(
        invalidControlType);
    if (!RequireSelfTest(
            !controlTypeResult &&
                controlTypeResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::InvalidEnumValue,
            "control-type-after-crc"))
    {
        return 1;
    }

    const std::array<std::byte, 1> oneBytePayload{std::byte{0x5A}};
    const pbprotocol::ControlFragmentView minimumFragment{
        1, 0, 2, 30, 0, oneBytePayload};
    std::array<std::byte, pbprotocol::kMinimumControlFragmentBytes>
        minimumFragmentBytes{};
    if (!RequireSelfTest(
            pbprotocol::SerializeControlFragment(
                minimumFragment,
                minimumFragmentBytes) &&
                pbprotocol::ParseControlFragment(minimumFragmentBytes),
            "minimum-fragment"))
    {
        return 1;
    }

    const std::vector<std::byte> maximumFragmentPayload(
        pbprotocol::kMaximumControlFragmentPayloadBytes,
        std::byte{0x6B});
    const pbprotocol::ControlFragmentView maximumFragment{
        2,
        0,
        1,
        static_cast<std::uint32_t>(maximumFragmentPayload.size()),
        0,
        maximumFragmentPayload};
    std::vector<std::byte> maximumFragmentBytes(
        pbprotocol::kMaximumControlFragmentBytes);
    if (!RequireSelfTest(
            pbprotocol::SerializeControlFragment(
                maximumFragment,
                maximumFragmentBytes) &&
                pbprotocol::ParseControlFragment(maximumFragmentBytes),
            "maximum-fragment"))
    {
        return 1;
    }

    auto invalidFragmentFlags = minimumFragmentBytes;
    StoreUint16(invalidFragmentFlags, 18, 1);
    RefreshCrc(
        invalidFragmentFlags,
        pbprotocol::kControlFragmentCrcBytes);
    const auto fragmentFlagsResult = pbprotocol::ParseControlFragment(
        invalidFragmentFlags);
    if (!RequireSelfTest(
            !fragmentFlagsResult &&
                fragmentFlagsResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
            "fragment-flags-after-crc"))
    {
        return 1;
    }

    const std::vector<std::byte> truncatedFragmentBytes(
        minimumFragmentBytes.begin(),
        minimumFragmentBytes.end() - 1);
    const auto truncatedFragmentResult = pbprotocol::ParseControlFragment(
        truncatedFragmentBytes);
    if (!RequireSelfTest(
            !truncatedFragmentResult &&
                truncatedFragmentResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::TruncatedInput,
            "truncated-fragment"))
    {
        return 1;
    }

    std::vector<std::byte> trailingFragmentBytes(
        minimumFragmentBytes.begin(),
        minimumFragmentBytes.end());
    trailingFragmentBytes.push_back(std::byte{0});
    const auto trailingFragmentResult = pbprotocol::ParseControlFragment(
        trailingFragmentBytes);
    if (!RequireSelfTest(
            !trailingFragmentResult &&
                trailingFragmentResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::InvalidRecordSize,
            "trailing-fragment"))
    {
        return 1;
    }

    const pbprotocol::ControlRecordView minimumRecord{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        1,
        pbprotocol::SessionTag{2},
        {}};
    std::array<std::byte, pbprotocol::kMinimumControlRecordBytes>
        minimumRecordBytes{};
    if (!RequireSelfTest(
            pbprotocol::SerializeControlRecord(
                minimumRecord,
                minimumRecordBytes) &&
                pbprotocol::ParseControlRecord(minimumRecordBytes),
            "minimum-control-record"))
    {
        return 1;
    }

    const std::vector<std::byte> maximumPayload(
        pbprotocol::kMaximumControlPayloadBytes,
        std::byte{0xA5});
    const pbprotocol::ControlRecordView maximumRecord{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        1,
        pbprotocol::SessionTag{2},
        maximumPayload};
    std::vector<std::byte> maximumRecordBytes(
        pbprotocol::kMaximumControlRecordBytes);
    if (!RequireSelfTest(
            pbprotocol::SerializeControlRecord(
                maximumRecord,
                maximumRecordBytes) &&
                pbprotocol::ParseControlRecord(maximumRecordBytes),
            "maximum-control-record"))
    {
        return 1;
    }
    maximumRecordBytes.push_back(std::byte{0});
    const auto oversizedControlResult = pbprotocol::ParseControlRecord(
        maximumRecordBytes);
    if (!RequireSelfTest(
            !oversizedControlResult &&
                oversizedControlResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::LengthLimitExceeded,
            "oversized-control-record"))
    {
        return 1;
    }

    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!RequireSelfTest(
            static_cast<bool>(receiverResult),
            "receiver-create"))
    {
        return 1;
    }
    auto receiver = std::move(receiverResult).Value();
    const auto fragments = MakeSeedFragments(0x0102030405060708ULL);
    const auto storedTail = receiver.ReceiveControlFragment(fragments[2], 1);
    const auto storedHead = receiver.ReceiveControlFragment(fragments[0], 2);
    const auto admission = receiver.ReceiveControlFragment(fragments[1], 3);
    if (!RequireSelfTest(
            storedTail && storedHead && admission &&
                admission.Value().disposition ==
                    pbprotocol::ControlFragmentReceiveDisposition::
                        DescriptorInserted &&
                receiver.ActiveSessionCount() == 1,
            "out-of-order-admission"))
    {
        return 1;
    }
    const auto repeated = receiver.ReceiveControlFragment(fragments[0], 4);
    if (!RequireSelfTest(
            repeated &&
                repeated.Value().disposition ==
                    pbprotocol::ControlFragmentReceiveDisposition::Repeated &&
                receiver.ActiveSessionCount() == 1,
            "completed-id-once"))
    {
        return 1;
    }

    auto conflictReceiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!conflictReceiverResult)
    {
        return 1;
    }
    auto conflictReceiver = std::move(conflictReceiverResult).Value();
    if (!conflictReceiver.ReceiveControlFragment(fragments[0], 1))
    {
        return 1;
    }
    const auto parsedFragment = pbprotocol::ParseControlFragment(fragments[0]);
    if (!parsedFragment)
    {
        return 1;
    }
    std::vector<std::byte> changedPayload(
        parsedFragment.Value().payload.begin(),
        parsedFragment.Value().payload.end());
    changedPayload[0] ^= std::byte{0x01};
    pbprotocol::ControlFragmentView changedFragment = parsedFragment.Value();
    changedFragment.payload = changedPayload;
    std::vector<std::byte> changedFragmentBytes(fragments[0].size());
    if (!pbprotocol::SerializeControlFragment(
            changedFragment,
            changedFragmentBytes))
    {
        return 1;
    }
    const auto conflict = conflictReceiver.ReceiveControlFragment(
        changedFragmentBytes,
        2);
    if (!RequireSelfTest(
            !conflict &&
                conflict.Error().code ==
                    pbprotocol::ProtocolErrorCode::ControlFragmentConflict,
            "conflicting-duplicate"))
    {
        return 1;
    }

    pbprotocol::ReceiverResourcePolicy tightPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    tightPolicy.maxConcurrentControlReassemblies = 1;
    tightPolicy.maxControlReassemblyInactivityObservations = 2;
    auto tightReceiverResult = pbprotocol::ControlPlaneReceiver::Create(
        tightPolicy);
    if (!tightReceiverResult)
    {
        return 1;
    }
    auto tightReceiver = std::move(tightReceiverResult).Value();
    const auto otherFragments = MakeSeedFragments(2);
    if (!tightReceiver.ReceiveControlFragment(fragments[0], 10))
    {
        return 1;
    }
    const auto quotaResult = tightReceiver.ReceiveControlFragment(
        otherFragments[0],
        10);
    const bool exactBoundaryRetained =
        tightReceiver.AdvanceObservationOrdinal(12) &&
        tightReceiver.ActiveControlReassemblyCount() == 1;
    const bool expiredAfterBoundary =
        tightReceiver.AdvanceObservationOrdinal(13) &&
        tightReceiver.ActiveControlReassemblyCount() == 0;
    const auto resetStoredResult = tightReceiver.ReceiveControlFragment(
        fragments[0],
        14);
    const bool hadStateBeforeReset =
        tightReceiver.ActiveControlReassemblyCount() == 1;
    tightReceiver.ResetControlReassembly();
    const bool resetClearsWindow = resetStoredResult && hadStateBeforeReset &&
        tightReceiver.ActiveControlReassemblyCount() == 0;
    if (!RequireSelfTest(
            !quotaResult &&
                quotaResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::
                        ControlReassemblyQuotaExceeded &&
                exactBoundaryRetained && expiredAfterBoundary &&
                resetClearsWindow,
            "quota-expiry-and-reset"))
    {
        return 1;
    }

    auto wrongTagRecord = kControlSeed;
    wrongTagRecord[14] ^= std::byte{0x01};
    RefreshCrc(wrongTagRecord, pbprotocol::kControlRecordCrcBytes);
    auto wrongTagReceiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!wrongTagReceiverResult)
    {
        return 1;
    }
    auto wrongTagReceiver = std::move(wrongTagReceiverResult).Value();
    const auto wrongTagResult = wrongTagReceiver.ReceiveControlRecord(
        wrongTagRecord);
    if (!RequireSelfTest(
            !wrongTagResult &&
                wrongTagResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::SessionTagMismatch &&
                wrongTagReceiver.ActiveSessionCount() == 0,
            "authoritative-session-tag"))
    {
        return 1;
    }

    std::array<std::byte, 38> mismatchedPayload{};
    std::copy_n(kControlSeed.begin() + 26, 37, mismatchedPayload.begin());
    const pbprotocol::ControlRecordView mismatchedRecord{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        3,
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL},
        mismatchedPayload};
    std::vector<std::byte> mismatchedRecordBytes(
        pbprotocol::kMinimumControlRecordBytes + mismatchedPayload.size());
    if (!pbprotocol::SerializeControlRecord(
            mismatchedRecord,
            mismatchedRecordBytes))
    {
        return 1;
    }
    auto mismatchReceiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!mismatchReceiverResult)
    {
        return 1;
    }
    auto mismatchReceiver = std::move(mismatchReceiverResult).Value();
    const auto mismatchResult = mismatchReceiver.ReceiveControlRecord(
        mismatchedRecordBytes);
    if (!RequireSelfTest(
            !mismatchResult &&
                mismatchResult.Error().code ==
                    pbprotocol::ProtocolErrorCode::TrailingBytes &&
                mismatchReceiver.ActiveSessionCount() == 0,
            "type-payload-no-fallback"))
    {
        return 1;
    }

    if (!RequireSelfTest(
            ExerciseStructuredDescriptorConflict(),
            "descriptor-conflict-terminal"))
    {
        return 1;
    }

    if (!RequireSelfTest(
            ExerciseStructuredExtremeMetadata({}),
            "extreme-fragment-count-and-total-record-bytes"))
    {
        return 1;
    }

    std::cout << "STRUCTURED_SELF_TEST_COMPLETED\n";
    return 0;
}

template <std::size_t SourceBytes>
void CopySeed(
    const std::array<std::byte, SourceBytes>& source,
    const std::span<std::byte> destination,
    std::size_t& destinationSize)
{
    std::copy(source.begin(), source.end(), destination.begin());
    destinationSize = source.size();
}

int RunMutationLoop(
    const std::uint64_t iterations,
    const std::uint64_t initialSeed)
{
    std::uint64_t randomState = initialSeed == 0 ? kDefaultSeed : initialSeed;
    std::vector<std::byte> input(kMaximumAcceptedFuzzInputBytes);

    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        std::size_t inputSize = 0;
        switch (NextRandom(randomState) % 5ULL)
        {
        case 0:
            CopySeed(kBootstrapSeed, input, inputSize);
            break;
        case 1:
            CopySeed(kControlSeed, input, inputSize);
            break;
        case 2:
            inputSize = static_cast<std::size_t>(
                NextRandom(randomState) % (kMaximumRandomInputBytes + 1ULL));
            for (std::size_t byteIndex = 0; byteIndex < inputSize; byteIndex++)
            {
                input[byteIndex] = static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
            break;
        case 3:
            inputSize = 0;
            break;
        default:
            inputSize = kMaximumAcceptedFuzzInputBytes;
            break;
        }

        if (inputSize != 0)
        {
            const std::size_t mutationCount = static_cast<std::size_t>(
                NextRandom(randomState) % 9ULL);
            for (std::size_t mutationIndex = 0;
                 mutationIndex < mutationCount;
                 mutationIndex++)
            {
                const std::size_t byteIndex = static_cast<std::size_t>(
                    NextRandom(randomState) % inputSize);
                input[byteIndex] ^= static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
        }

        ExerciseInput(std::span<const std::byte>(input).first(inputSize));
    }

    std::cout << "FUZZ_COMPLETED iterations=" << iterations
              << " seed=" << initialSeed << '\n';
    return 0;
}

int ReplayInputFile(const std::string_view inputPath)
{
    std::ifstream inputFile(std::string(inputPath), std::ios::binary);
    if (!inputFile)
    {
        std::cerr << "CORPUS_REPLAY_OPEN_FAILED path=" << inputPath << '\n';
        return 2;
    }

    std::vector<char> input(kMaximumAcceptedFuzzInputBytes);
    inputFile.read(
        input.data(),
        static_cast<std::streamsize>(input.size()));
    const std::streamsize inputSize = inputFile.gcount();
    if (inputFile.bad())
    {
        std::cerr << "CORPUS_REPLAY_READ_FAILED path=" << inputPath << '\n';
        return 2;
    }

    if (inputSize == static_cast<std::streamsize>(input.size()))
    {
        char extraByte = 0;
        inputFile.read(&extraByte, 1);
        if (inputFile.gcount() != 0)
        {
            std::cerr << "CORPUS_REPLAY_INPUT_TOO_LARGE path="
                      << inputPath << '\n';
            return 2;
        }
        if (inputFile.bad())
        {
            std::cerr << "CORPUS_REPLAY_READ_FAILED path="
                      << inputPath << '\n';
            return 2;
        }
    }

    const std::size_t inputByteCount = static_cast<std::size_t>(inputSize);
    ExerciseInput(std::as_bytes(std::span(input).first(inputByteCount)));
    std::cout << "CORPUS_REPLAY_NO_CRASH path=" << inputPath
              << " bytes=" << inputByteCount << '\n';
    return 0;
}

#endif

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* const data,
    const std::size_t size)
{
    ExerciseInput(std::as_bytes(std::span(data, size)));
    return 0;
}

#if !defined(PB_USE_LIBFUZZER)

int main(const int argumentCount, char* arguments[])
{
    if (argumentCount == 2 &&
        std::string_view(arguments[1]) == "--self-test")
    {
        return RunStructuredSelfTest();
    }
    if (argumentCount == 3 &&
        std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }

    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(arguments[1], iterations))
    {
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n"
                     "       PBProtocolBootstrapControlFuzz --self-test\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n"
                     "       PBProtocolBootstrapControlFuzz --self-test\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n"
                     "       PBProtocolBootstrapControlFuzz --self-test\n";
        return 2;
    }

    return RunMutationLoop(iterations, seed);
}

#endif
