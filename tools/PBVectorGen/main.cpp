// PBVectorGen: deterministic golden vector and fuzz corpus generator.
//
// Commands:
//   write-golden <dir> [--only <category>]
//       Recomputes every registry vector (optionally filtered by category)
//       with the current implementation, verifies the recomputed BLAKE3
//       against the pinned registry digest, and writes
//       <dir>/<category>/<name>.bin. A digest drift refuses the write
//       (fail-closed: regeneration is a reviewed act, see
//       docs/GOLDEN_VECTOR_HARNESS.md).
//   write-corpus <dir> --category <transport|interleave|ldpc> [--seed <v>]
//       Writes the deterministic fuzz corpus seeds for one new driver into
//       <dir>/<category>/.
//   write-frame <file> --vector <g0|g1|g1-transport|g1-transport-2cw|g2>
//       --format <pbrw|png>
//       Renders one full reference frame after checking its frozen digest.
//   dump-manifest [--skip-frames]
//       Prints the deterministic manifest (recomputed vs pinned digests).
//
// Exit codes: 0 = success, 1 = content/digest failure, 2 = usage/IO error.
// stdout is deterministic (no timestamps, fixed ordering).

#include "pbgolden/golden_vector_registry.h"
#include "pbgolden/golden_vector_source.h"
#include "pbinterleave/interleave_reference.h"
#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

[[nodiscard]] std::string ToHexLower(
    const std::span<const std::byte> data)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2u);
    for (const std::byte value : data)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        result.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
        result.push_back(kHexDigits[byteValue & 0xFu]);
    }
    return result;
}

enum class WriteResult
{
    Written,
    Identical,
    Conflict,
    IoError
};

struct Artifact
{
    std::string name;
    std::vector<std::byte> bytes;
};

[[nodiscard]] WriteResult WriteFile(
    const std::filesystem::path& path,
    const std::span<const std::byte> data)
{
    std::error_code errorCode;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, errorCode);
        if (errorCode)
        {
            return WriteResult::IoError;
        }
    }
    if (std::filesystem::exists(path, errorCode))
    {
        if (errorCode)
        {
            return WriteResult::IoError;
        }
        std::ifstream existing(path, std::ios::binary);
        if (!existing)
        {
            return WriteResult::IoError;
        }
        existing.seekg(0, std::ios::end);
        const std::streamoff existingSize = existing.tellg();
        if (existingSize < 0 || static_cast<std::uint64_t>(existingSize) != data.size())
        {
            return existingSize < 0 ? WriteResult::IoError : WriteResult::Conflict;
        }
        existing.seekg(0, std::ios::beg);
        std::vector<std::byte> existingBytes(data.size());
        if (!existingBytes.empty())
        {
            existing.read(reinterpret_cast<char*>(existingBytes.data()),
                static_cast<std::streamsize>(existingBytes.size()));
        }
        if (!existing)
        {
            return WriteResult::IoError;
        }
        return std::equal(existingBytes.begin(), existingBytes.end(), data.begin(), data.end())
            ? WriteResult::Identical : WriteResult::Conflict;
    }
    std::ofstream stream(
        path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return WriteResult::IoError;
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    if (!stream)
    {
        return WriteResult::IoError;
    }
    stream.close();
    if (!stream)
    {
        return WriteResult::IoError;
    }
    std::ifstream verify(path, std::ios::binary);
    if (!verify)
    {
        return WriteResult::IoError;
    }
    verify.seekg(0, std::ios::end);
    const std::streamoff verifiedSize = verify.tellg();
    if (verifiedSize < 0 ||
        static_cast<std::uint64_t>(verifiedSize) != data.size())
    {
        return WriteResult::IoError;
    }
    verify.seekg(0, std::ios::beg);
    std::vector<std::byte> verifiedBytes(data.size());
    if (!verifiedBytes.empty())
    {
        verify.read(reinterpret_cast<char*>(verifiedBytes.data()),
            static_cast<std::streamsize>(verifiedBytes.size()));
    }
    if (!verify ||
        !std::equal(verifiedBytes.begin(), verifiedBytes.end(), data.begin(), data.end()))
    {
        return WriteResult::IoError;
    }
    return WriteResult::Written;
}

[[nodiscard]] std::uint32_t ReadStoredCrc(
    const std::span<const std::byte> bytes,
    const std::size_t crcOffset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[crcOffset + byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

void StoreCrc(
      std::span<std::byte> bytes,
    const std::size_t crcOffset,
    const std::uint32_t value) noexcept
{
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        bytes[crcOffset + byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>(
                (value >> static_cast<unsigned int>(byteIndex * 8U)) &
                0xFFU));
    }
}

void StoreU16(std::span<std::byte> bytes, const std::size_t offset,
    const std::uint16_t value) noexcept
{
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value & 0xFFu)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void RefreshTransportHeaderCrc(std::vector<std::byte>& bytes)
{
    const std::uint32_t crc = static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(pbprotocol::kTransportHeaderCrcCoverageBytes)));
    StoreCrc(bytes, pbprotocol::kTransportHeaderCrcOffset, crc);
}

// ---------------------------------------------------------------------------
// write-golden
// ---------------------------------------------------------------------------

[[nodiscard]] int RunWriteGolden(
    const std::filesystem::path& root,
    const std::string& onlyCategory)
{
    // An empty category means all categories (--only is optional in the
    // documented CLI contract).
    if (!onlyCategory.empty() && onlyCategory != "protocol" &&
        onlyCategory != "ldpc" && onlyCategory != "interleave" &&
        onlyCategory != "raster")
    {
        std::cout <<
            "[PBVectorGen] ERROR: unknown category '" << onlyCategory
            << "' (expected protocol|ldpc|interleave|raster)\n";
        return 2;
    }
    int failureCount = 0;
    bool ioFailure = false;
    for (const auto& descriptor : pbgolden::GetGoldenVectorRegistry())
    {
        if (!onlyCategory.empty() &&
            descriptor.category != onlyCategory)
        {
            continue;
        }
        const auto outcome =
            pbgolden::RecomputeGoldenVector(descriptor.name);
        if (outcome.status != pbgolden::RecomputeStatus::Ok)
        {
            std::cout << "[PBVectorGen] FAIL vector=" << descriptor.name
                << " recompute=" << outcome.detail << "\n";
            failureCount++;
            continue;
        }
        const auto digest =
            pbprotocol::ComputeBlake3Digest(outcome.bytes);
        if (digest != descriptor.blake3)
        {
            // Fail-closed: a drift against the pinned digest means the
            // generator changed without a reviewed re-pin.
            std::cout << "[PBVectorGen] FAIL vector=" << descriptor.name
                << " digest drift (refusing to write): recomputed="
                << ToHexLower(digest) << " pinned="
                << ToHexLower(descriptor.blake3) << "\n";
            failureCount++;
            continue;
        }
        if (outcome.bytes.size() != descriptor.sizeBytes)
        {
            std::cout << "[PBVectorGen] FAIL vector=" << descriptor.name
                << " byte_offset=" << std::min(outcome.bytes.size(), descriptor.sizeBytes)
                << " expected_size=" << descriptor.sizeBytes
                << " actual_size=" << outcome.bytes.size() << "\n";
            failureCount++;
            continue;
        }
        const auto file = root / std::string(descriptor.category) /
            (std::string(descriptor.name) + ".bin");
        const WriteResult writeResult = WriteFile(file, outcome.bytes);
        if (writeResult == WriteResult::Conflict)
        {
            std::cout << "[PBVectorGen] FAIL vector=" << descriptor.name
                << " target-content-conflict (refusing to overwrite) "
                << file.generic_string() << "\n";
            failureCount++;
            continue;
        }
        if (writeResult == WriteResult::IoError)
        {
            std::cout << "[PBVectorGen] FAIL vector=" << descriptor.name
                << " io error writing " << file.generic_string() << "\n";
            ioFailure = true;
            continue;
        }
        std::cout << "[PBVectorGen] "
            << (writeResult == WriteResult::Identical ? "IDENTICAL " : "WROTE ")
            << file.generic_string()
            << " (" << outcome.bytes.size() << " bytes, blake3="
            << ToHexLower(digest) << ")\n";
    }
    return ioFailure ? 2 : (failureCount == 0 ? 0 : 1);
}

// ---------------------------------------------------------------------------
// write-corpus
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<Artifact> MakeCorpusTransport(
    const std::uint64_t seed)
{
    std::vector<Artifact> artifacts;
    const auto canonical = pbgolden::GenerateTransportBlockCanonical();
    const auto minimum = pbgolden::GenerateTransportBlockMinimum();
    pbgolden::SplitMix64 rng(seed);
    const auto pickOffset = [nextValue = &rng](
        const std::size_t limit) {
        return static_cast<std::size_t>(nextValue->Next() % limit);
    };

    artifacts.push_back({"valid-minimum.bin", minimum});
    artifacts.push_back({"valid-canonical.bin", canonical});
    artifacts.push_back({"valid-maximum.bin", pbgolden::GenerateTransportBlockMaxPayload()});
    artifacts.push_back({"truncated-header.bin",
        std::vector<std::byte>(canonical.begin(), canonical.begin() + 31)});
    artifacts.push_back({"truncated-payload.bin",
        std::vector<std::byte>(canonical.begin(), canonical.end() - 1)});
    {
        auto variant = minimum;
        variant.push_back(std::byte{0});
        artifacts.push_back({"trailing-byte.bin", std::move(variant)});
    }
    // Corrupted header CRC (payload CRC remains valid): parsing must stop at
    // the header CRC gate.
    {
        auto variant = canonical;
        const auto storedCrc = ReadStoredCrc(
            std::span<const std::byte>(variant),
            pbprotocol::kTransportHeaderCrcOffset);
        StoreCrc(
            std::span<std::byte>(variant),
            pbprotocol::kTransportHeaderCrcOffset,
            storedCrc ^ 0xDEADBEEFU);
        artifacts.push_back({"header-crc-corrupt.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        const auto payloadOffset = pbprotocol::kTransportPayloadOffset +
            pickOffset(canonical.size() - pbprotocol::kTransportPayloadOffset - 4);
        variant[payloadOffset] =
            static_cast<std::byte>(
                std::to_integer<std::uint8_t>(variant[payloadOffset]) ^
                0x01U);
        artifacts.push_back({"payload-crc-corrupt.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        variant[0] = std::byte{2};
        RefreshTransportHeaderCrc(variant);
        artifacts.push_back({"semantic-unsupported-type.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        variant[1] = std::byte{1};
        RefreshTransportHeaderCrc(variant);
        artifacts.push_back({"semantic-unsupported-minor.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        variant[2] = std::byte{1};
        RefreshTransportHeaderCrc(variant);
        artifacts.push_back({"semantic-nonzero-flags.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        variant[pbprotocol::kTransportReservedOffset] = std::byte{1};
        RefreshTransportHeaderCrc(variant);
        artifacts.push_back({"semantic-nonzero-reserved.bin", std::move(variant)});
    }
    {
        auto variant = canonical;
        StoreU16(variant, pbprotocol::kTransportPayloadBytesOffset, 1);
        RefreshTransportHeaderCrc(variant);
        artifacts.push_back({"semantic-declared-length-small.bin", std::move(variant)});
    }
    {
        std::vector<std::byte> infoBlock(1350, std::byte{0});
        std::copy(minimum.begin(), minimum.end(), infoBlock.begin());
        artifacts.push_back({"info-block-zero-padding.bin", infoBlock});
        infoBlock.back() = std::byte{1};
        artifacts.push_back({"info-block-dirty-padding.bin", std::move(infoBlock)});
    }
    return artifacts;
}

[[nodiscard]] std::vector<Artifact> MakeCorpusInterleave(
    const std::uint64_t seed)
{
    (void)seed;
    std::vector<Artifact> artifacts;
    auto zeros = std::vector<std::byte>(
        pbinterleave::kInterleaveRegionBytes, std::byte{0});
    artifacts.push_back({"valid-zero-region.bin", std::move(zeros)});
    artifacts.push_back({"valid-logical-pattern.bin", pbgolden::GenerateInterleaveRegionLogical()});
    artifacts.push_back({"valid-physical-phase7.bin", pbgolden::GenerateInterleaveRegionPhysicalPhase7()});
    auto shortInput = pbgolden::GenerateInterleaveRegionLogical();
    shortInput.pop_back();
    artifacts.push_back({"invalid-short-region.bin", std::move(shortInput)});
    auto longInput = pbgolden::GenerateInterleaveRegionLogical();
    longInput.push_back(std::byte{0});
    artifacts.push_back({"invalid-long-region.bin", std::move(longInput)});
    return artifacts;
}

[[nodiscard]] std::vector<Artifact> MakeCorpusLdpc(const std::uint64_t seed)
{
    pbgolden::SplitMix64 rng(seed);
    std::vector<Artifact> artifacts;
    const auto golden = pbgolden::GenerateLdpcCodewordRobust();
    const std::size_t codewordBytes = golden.size();
    const auto flipBitAt = [&golden](
        const std::size_t bitOffset) {
        auto variant = golden;
        const auto byteValue =
            std::to_integer<std::uint8_t>(variant[bitOffset / 8]);
        variant[bitOffset / 8] = static_cast<std::byte>(
            byteValue ^ static_cast<std::uint8_t>(1U << (bitOffset % 8)));
        return variant;
    };
    artifacts.push_back({"valid-robust.bin", golden});
    artifacts.push_back({"valid-balanced.bin", pbgolden::GenerateLdpcCodewordBalanced()});
    artifacts.push_back({"valid-fast.bin", pbgolden::GenerateLdpcCodewordFast()});
    // 2: single-bit flip in the systematic region.
    {
        const auto variant =
            flipBitAt(static_cast<std::size_t>(
                rng.Next() % 10000));
        artifacts.push_back({"single-bit-systematic.bin", std::move(variant)});
    }
    // 3: single-bit flip at the K-1 systematic/parity boundary bit.
    {
        const auto variant = flipBitAt(10800 - 1);
        artifacts.push_back({"single-bit-k-minus-one.bin", std::move(variant)});
    }
    // 4: single-bit flip at the N-1 (last parity) bit.
    {
        const auto variant = flipBitAt(16200 - 1);
        artifacts.push_back({"single-bit-n-minus-one.bin", std::move(variant)});
    }
    // 5: eight-byte segment flip in the parity region.
    {
        auto variant = golden;
        const auto segmentOffset =
            1350 + static_cast<std::size_t>(rng.Next() % (codewordBytes - 1350 - 8));
        for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
        {
            variant[segmentOffset + byteIndex] =
                static_cast<std::byte>(
                    std::to_integer<std::uint8_t>(
                        variant[segmentOffset + byteIndex]) ^
                    0xA5U);
        }
        artifacts.push_back({"parity-segment-flip.bin", std::move(variant)});
    }
    artifacts.push_back({"invalid-truncated.bin",
        std::vector<std::byte>(golden.begin(), golden.end() - 1)});
    return artifacts;
}

[[nodiscard]] int RunWriteCorpus(
    const std::filesystem::path& root,
    const std::string& category,
    const std::uint64_t seed)
{
    if (category != "transport" && category != "interleave" && category != "ldpc")
    {
        std::cout << "[PBVectorGen] ERROR: unknown corpus category '" << category << "'\n";
        return 2;
    }
    const auto artifacts = category == "transport" ? MakeCorpusTransport(seed)
        : category == "interleave" ? MakeCorpusInterleave(seed) : MakeCorpusLdpc(seed);
    const std::size_t expectedArtifactCount = category == "transport" ? 15u
        : category == "interleave" ? 5u : 8u;
    if (artifacts.size() != expectedArtifactCount)
    {
        std::cout << "[PBVectorGen] FAIL corpus=" << category
            << " artifact-count expected=" << expectedArtifactCount
            << " actual=" << artifacts.size() << "\n";
        return 1;
    }
    for (std::size_t artifactIndex = 0;
        artifactIndex < artifacts.size(); artifactIndex++)
    {
        const Artifact& artifact = artifacts[artifactIndex];
        if (artifact.name.empty() || artifact.bytes.empty())
        {
            std::cout << "[PBVectorGen] FAIL corpus=" << category
                << " invalid artifact\n";
            return 1;
        }
        for (std::size_t previousIndex = 0;
            previousIndex < artifactIndex; previousIndex++)
        {
            if (artifacts[previousIndex].name == artifact.name)
            {
                std::cout << "[PBVectorGen] FAIL corpus=" << category
                    << " duplicate artifact=" << artifact.name << "\n";
                return 1;
            }
        }
        const auto file = root / category / artifact.name;
        const WriteResult writeResult = WriteFile(file, artifact.bytes);
        if (writeResult == WriteResult::Conflict)
        {
            std::cout << "[PBVectorGen] FAIL corpus=" << category
                << " target-content-conflict (refusing to overwrite) "
                << file.generic_string() << "\n";
            return 1;
        }
        if (writeResult == WriteResult::IoError)
        {
            std::cout << "[PBVectorGen] ERROR corpus=" << category
                << " io error writing " << file.generic_string() << "\n";
            return 2;
        }
        const auto digest = pbprotocol::ComputeBlake3Digest(artifact.bytes);
        std::cout << "[PBVectorGen] "
            << (writeResult == WriteResult::Identical ? "IDENTICAL " : "WROTE ")
            << file.generic_string() << " (" << artifact.bytes.size()
            << " bytes, blake3=" << ToHexLower(digest) << ")\n";
    }
    return 0;
}

[[nodiscard]] const pbgolden::FrameVectorPin* FindFramePin(
    const std::string& vectorName)
{
    const std::string registryName = vectorName == "g0" ? "g0-zero"
        : vectorName == "g1" ? "g1-canonical"
        : vectorName == "g2" ? "g2-max" : vectorName;
    for (const auto& pin : pbgolden::GetFrameVectorRegistry())
    {
        if (pin.name == registryName)
        {
            return &pin;
        }
    }
    return nullptr;
}

[[nodiscard]] pbgolden::GoldenFramePayload MakeFramePayload(
    const pbgolden::FrameVectorId id)
{
    switch (id)
    {
    case pbgolden::FrameVectorId::G0Zero:
        return pbgolden::MakeZeroFramePayload();
    case pbgolden::FrameVectorId::G1Canonical:
        return pbgolden::MakeCanonicalFramePayload();
    case pbgolden::FrameVectorId::G1Transport:
        return pbgolden::MakeG1TransportFramePayload();
    case pbgolden::FrameVectorId::G1Transport2Cw:
        return pbgolden::MakeG1Transport2CwFramePayload();
    case pbgolden::FrameVectorId::G2Max:
        return pbgolden::MakeMaxFramePayload();
    }
    throw std::invalid_argument("unknown frame vector id");
}

[[nodiscard]] int RunWriteFrame(const std::filesystem::path& output,
    const std::string& vectorName, const std::string& format)
{
    const auto* pin = FindFramePin(vectorName);
    if (pin == nullptr || (format != "pbrw" && format != "png"))
    {
        std::cout << "[PBVectorGen] ERROR: invalid frame vector or format\n";
        return 2;
    }
    const auto payload = MakeFramePayload(pin->id);
    std::vector<std::byte> canvas(pbmodulation::kReferenceFrameBgraBytes);
    pbmodulation::ReferenceFrameInput input;
    input.bootstrapRecord = payload.bootstrap;
    input.controlWindow = payload.control;
    input.data = payload.data;
    const auto rasterStatus = pbmodulation::EncodeReferenceFrame(input, canvas);
    if (!rasterStatus)
    {
        std::cout << "[PBVectorGen] FAIL frame=" << vectorName
            << " raster-error=" << static_cast<unsigned int>(rasterStatus.Error().code)
            << " offset=" << rasterStatus.Error().offset << "\n";
        return 1;
    }
    auto encodedResult = format == "pbrw"
        ? pbmodulation::EncodeRawFrame(canvas, pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight)
        : pbmodulation::EncodePngFrame(canvas, pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight);
    if (!encodedResult)
    {
        std::cout << "[PBVectorGen] FAIL frame=" << vectorName
            << " container-error=" << static_cast<unsigned int>(encodedResult.Error().code)
            << " offset=" << encodedResult.Error().offset << "\n";
        return 1;
    }
    const auto& encoded = encodedResult.Value();
    const auto digest = pbprotocol::ComputeBlake3Digest(encoded);
    const auto& pinnedDigest = format == "pbrw" ? pin->rawBlake3 : pin->pngBlake3;
    if (digest != pinnedDigest)
    {
        std::cout << "[PBVectorGen] FAIL frame=" << vectorName
            << " digest-drift byte_offset=not-applicable expected=blake3:"
            << ToHexLower(pinnedDigest) << " actual=blake3:" << ToHexLower(digest) << "\n";
        return 1;
    }
    const WriteResult writeResult = WriteFile(output, encoded);
    if (writeResult == WriteResult::Conflict)
    {
        std::cout << "[PBVectorGen] FAIL frame=" << vectorName
            << " target-content-conflict (refusing to overwrite) "
            << output.generic_string() << "\n";
        return 1;
    }
    if (writeResult == WriteResult::IoError)
    {
        std::cout << "[PBVectorGen] ERROR frame=" << vectorName
            << " io error writing " << output.generic_string() << "\n";
        return 2;
    }
    std::cout << "[PBVectorGen] "
        << (writeResult == WriteResult::Identical ? "IDENTICAL " : "WROTE ")
        << output.generic_string() << " (" << encoded.size()
        << " bytes, blake3=" << ToHexLower(digest) << ")\n";
    return 0;
}

[[nodiscard]] bool ParseSeed(const std::string_view text,
    std::uint64_t& outSeed) noexcept
{
    if (text.empty() || text.front() == '+' || text.front() == '-')
    {
        return false;
    }
    int base = 10;
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' &&
        (digits[1] == 'x' || digits[1] == 'X'))
    {
        base = 16;
        digits.remove_prefix(2);
    }
    if (digits.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
    {
        return false;
    }
    outSeed = value;
    return true;
}

// ---------------------------------------------------------------------------
// dump-manifest
// ---------------------------------------------------------------------------

[[nodiscard]] int RunDumpManifest(const bool skipFrames)
{
    std::cout << "PBVectorGen manifest (deterministic, "
              << "recomputed vs pinned BLAKE3-256)\n" << std::flush;
    int failureCount = 0;
    for (const auto& descriptor : pbgolden::GetGoldenVectorRegistry())
    {
        const auto outcome =
            pbgolden::RecomputeGoldenVector(descriptor.name);
        if (outcome.status != pbgolden::RecomputeStatus::Ok)
        {
            std::cout << "vector=" << descriptor.name << " category="
                << descriptor.category << " size=" << descriptor.sizeBytes
                << " status=RECOMPUTE-FAILED detail=" << outcome.detail
                << "\n";
            failureCount++;
            continue;
        }
        const auto digest =
            pbprotocol::ComputeBlake3Digest(outcome.bytes);
        const bool pinnedNonZero =
            std::any_of(descriptor.blake3.begin(), descriptor.blake3.end(),
                [](const std::byte value) {
                    return std::to_integer<std::uint8_t>(value) != 0;
                });
        const std::string status = !pinnedNonZero
            ? "UNPINNED"
            : (digest == descriptor.blake3 ? "match" : "DRIFT");
        std::cout << "vector=" << descriptor.name << " category="
            << descriptor.category << " size=" << descriptor.sizeBytes
            << " recomputed_size=" << outcome.bytes.size()
            << " recomputed=blake3:" << ToHexLower(digest) << " pinned=blake3:"
            << ToHexLower(descriptor.blake3) << " status=" << status << "\n";
        if (status != "match" || outcome.bytes.size() != descriptor.sizeBytes)
        {
            failureCount++;
        }
        std::cout << std::flush;
    }
    if (!skipFrames)
    {
        for (const auto& pin : pbgolden::GetFrameVectorRegistry())
        {
            const auto payload = MakeFramePayload(pin.id);
            const auto result = pbgolden::ComputeFrameDigests(payload);
            if (!result.success)
            {
                std::cout << "frame=" << pin.name
                    << " status=RECOMPUTE-FAILED detail=" << result.detail
                    << "\n";
                failureCount++;
                continue;
            }
            const std::string rawStatus =
                result.rawBlake3 == pin.rawBlake3 ? "match" : "DRIFT";
            const std::string pngStatus =
                result.pngBlake3 == pin.pngBlake3 ? "match" : "DRIFT";
            std::cout << "frame=" << pin.name << " raw=blake3:"
                << ToHexLower(result.rawBlake3) << " pinned_raw=blake3:"
                << ToHexLower(pin.rawBlake3) << " status_raw=" << rawStatus
                << " png=blake3:" << ToHexLower(result.pngBlake3)
                << " pinned_png=blake3:" << ToHexLower(pin.pngBlake3)
                << " status_png=" << pngStatus << "\n";
            if (rawStatus == "DRIFT" || pngStatus == "DRIFT")
            {
                failureCount++;
            }
            std::cout << std::flush;
        }
    }
    std::cout << "manifest total="
        << (failureCount == 0 ? "all match" : std::to_string(failureCount)
            + " mismatch(es)") << "\n";
    return failureCount == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc < 2)
        {
            std::cout
                << "usage: PBVectorGen <write-golden|write-corpus|write-frame|dump-manifest> "
                   "[args]\n";
            return 2;
        }
        const std::string command = argv[1];
        if (command == "write-golden")
        {
            if (argc < 3)
            {
                std::cout << "usage: PBVectorGen write-golden <dir> "
                             "[--only <category>]\n";
                return 2;
            }
            std::filesystem::path root = argv[2];
            if (root.empty() || (!std::string(argv[2]).empty() && std::string(argv[2])[0] == '-'))
            {
                return 2;
            }
            std::string onlyCategory;
            bool onlyWasSet = false;
            for (int argIndex = 3; argIndex < argc; argIndex++)
            {
                const std::string arg = argv[argIndex];
                if (arg == "--only" && !onlyWasSet)
                {
                    if (argIndex + 1 >= argc)
                    {
                        return 2;
                    }
                    argIndex++;
                    onlyCategory = argv[argIndex];
                    onlyWasSet = true;
                }
                else
                {
                    return 2;
                }
            }
            return RunWriteGolden(root, onlyCategory);
        }
        if (command == "write-corpus")
        {
            std::filesystem::path root;
            std::string category;
            std::uint64_t seed = 0;
            bool hasSeed = false;
            bool categoryWasSet = false;
            bool rootWasSet = false;
            for (int argIndex = 2; argIndex < argc; argIndex++)
            {
                const std::string arg = argv[argIndex];
                if (arg == "--category" && !categoryWasSet)
                {
                    if (argIndex + 1 >= argc)
                    {
                        return 2;
                    }
                    argIndex++;
                    category = argv[argIndex];
                    categoryWasSet = true;
                }
                else if (arg == "--seed" && !hasSeed)
                {
                    if (argIndex + 1 >= argc)
                    {
                        return 2;
                    }
                    argIndex++;
                    if (!ParseSeed(argv[argIndex], seed))
                    {
                        return 2;
                    }
                    hasSeed = true;
                }
                else if (!arg.empty() && arg[0] != '-' && !rootWasSet)
                {
                    root = arg;
                    rootWasSet = true;
                }
                else
                {
                    return 2;
                }
            }
            if (root.empty() || category.empty() ||
                (category != "transport" && category != "interleave" && category != "ldpc"))
            {
                std::cout
                    << "usage: PBVectorGen write-corpus <dir> "
                       "--category <transport|interleave|ldpc> [--seed <v>]\n";
                return 2;
            }
            // Category defaults keep regeneration reproducible without a
            // seed argument.
            if (!hasSeed)
            {
                seed = category == "transport"
                    ? pbgolden::MakeTagSeed("PBTXFUZZ")
                    : category == "interleave"
                        ? pbgolden::MakeTagSeed("PBINTLFZ")
                        : pbgolden::MakeTagSeed("PBLDPFUX");
            }
            return RunWriteCorpus(root, category, seed);
        }
        if (command == "write-frame")
        {
            if (argc < 7)
            {
                std::cout << "usage: PBVectorGen write-frame <file> --vector "
                    "<g0|g1|g1-transport|g1-transport-2cw|g2> --format <pbrw|png>\n";
                return 2;
            }
            const std::filesystem::path output = argv[2];
            if (output.empty() || std::string(argv[2])[0] == '-')
            {
                return 2;
            }
            std::string vectorName;
            std::string format;
            bool vectorWasSet = false;
            bool formatWasSet = false;
            for (int argIndex = 3; argIndex < argc; argIndex++)
            {
                const std::string argument = argv[argIndex];
                if (argument == "--vector" && !vectorWasSet && argIndex + 1 < argc)
                {
                    argIndex++;
                    vectorName = argv[argIndex];
                    vectorWasSet = true;
                }
                else if (argument == "--format" && !formatWasSet && argIndex + 1 < argc)
                {
                    argIndex++;
                    format = argv[argIndex];
                    formatWasSet = true;
                }
                else
                {
                    return 2;
                }
            }
            if (!vectorWasSet || !formatWasSet)
            {
                return 2;
            }
            return RunWriteFrame(output, vectorName, format);
        }
        if (command == "dump-manifest")
        {
            bool skipFrames = false;
            for (int argIndex = 2; argIndex < argc; argIndex++)
            {
                if (std::string(argv[argIndex]) == "--skip-frames")
                {
                    if (skipFrames)
                    {
                        return 2;
                    }
                    skipFrames = true;
                }
                else
                {
                    return 2;
                }
            }
            return RunDumpManifest(skipFrames);
        }
        std::cout << "unknown command '" << command << "'\n";
        return 2;
    }
    catch (const std::exception& exception)
    {
        std::cout << "[PBVectorGen] ERROR: " << exception.what() << "\n";
        return 2;
    }
    catch (...)
    {
        std::cout << "[PBVectorGen] ERROR: unknown exception\n";
        return 2;
    }
}
