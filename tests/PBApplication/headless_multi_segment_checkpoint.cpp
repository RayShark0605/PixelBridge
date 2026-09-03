#include "local_desktop_runtime.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/protocol_types.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

inline constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireNtSuccess(const NTSTATUS status, const char* const operation)
{
    if (status < 0)
    {
        throw std::runtime_error(std::string(operation) + " failed with NTSTATUS " +
            std::to_string(static_cast<long>(status)));
    }
}

class BCryptAlgorithm final
{
public:
    BCryptAlgorithm()
    {
        RequireNtSuccess(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(SHA-256)");
    }

    ~BCryptAlgorithm()
    {
        if (handle_ != nullptr)
        {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptSha256Hash final
{
public:
    explicit BCryptSha256Hash(const BCRYPT_ALG_HANDLE algorithm)
    {
        DWORD copiedBytes = 0;
        DWORD objectBytes = 0;
        RequireNtSuccess(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copiedBytes, 0),
            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
        Require(copiedBytes == sizeof(objectBytes) && objectBytes != 0,
            "BCrypt returned an invalid SHA-256 object length");
        object_.resize(objectBytes);
        RequireNtSuccess(BCryptCreateHash(algorithm, &handle_, object_.data(), objectBytes,
            nullptr, 0, 0), "BCryptCreateHash(SHA-256)");
    }

    ~BCryptSha256Hash()
    {
        if (handle_ != nullptr)
        {
            BCryptDestroyHash(handle_);
        }
    }

    BCryptSha256Hash(const BCryptSha256Hash&) = delete;
    BCryptSha256Hash& operator=(const BCryptSha256Hash&) = delete;

    void Update(const std::span<const std::byte> bytes)
    {
        Require(bytes.size() <= (std::numeric_limits<ULONG>::max)(),
            "SHA-256 input chunk exceeds ULONG");
        RequireNtSuccess(BCryptHashData(handle_, reinterpret_cast<PUCHAR>(
            const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0),
            "BCryptHashData(SHA-256)");
    }

    [[nodiscard]] std::array<std::byte, 32> Finish()
    {
        std::array<std::byte, 32> digest{};
        RequireNtSuccess(BCryptFinishHash(handle_, reinterpret_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()), 0), "BCryptFinishHash(SHA-256)");
        return digest;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
    std::vector<UCHAR> object_;
};

enum class FixturePattern : std::uint8_t
{
    Deterministic,
    Repeating,
    Csprng
};

enum class CodecExpectation : std::uint8_t
{
    RawOnly,
    HasZstandard
};

struct FixtureDefinition
{
    const char* name = "";
    std::uint64_t byteCount = 0;
    FixturePattern pattern = FixturePattern::Deterministic;
    bool compressionEnabled = false;
    bool exerciseFourActiveBusy = false;
    CodecExpectation codecExpectation = CodecExpectation::RawOnly;
};

struct ExternalFileEvidence
{
    std::uint64_t byteCount = 0;
    std::array<std::byte, 32> sha256{};
    std::array<std::byte, pbprotocol::kDigestBytes> blake3{};
};

struct CaseEvidence
{
    FixtureDefinition definition;
    std::filesystem::path sourcePath;
    std::filesystem::path outputPath;
    ExternalFileEvidence source;
    ExternalFileEvidence output;
    pbapp::ApplicationHeadlessProbeSnapshot probe;
    bool byteExact = false;
};

[[nodiscard]] std::string BytesToHex(const std::span<const std::byte> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(bytes[index]);
        text[index * 2] = digits[byte >> 4U];
        text[index * 2 + 1] = digits[byte & 0x0FU];
    }
    return text;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    Require(value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "path is too long for UTF-8 conversion");
    const int sourceCharacters = static_cast<int>(value.size());
    const int requiredBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        sourceCharacters, nullptr, 0, nullptr, nullptr);
    Require(requiredBytes > 0, "path UTF-8 size conversion failed");
    std::string output(static_cast<std::size_t>(requiredBytes), '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceCharacters,
        output.data(), requiredBytes, nullptr, nullptr) == requiredBytes,
        "path UTF-8 conversion failed");
    return output;
}

[[nodiscard]] std::string JsonEscape(const std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 16);
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            Require(static_cast<unsigned char>(character) >= 0x20U,
                "JSON string contains an unsupported control character");
            output.push_back(character);
            break;
        }
    }
    return output;
}

[[nodiscard]] const char* GetPatternName(const FixturePattern pattern) noexcept
{
    switch (pattern)
    {
    case FixturePattern::Deterministic: return "deterministic-xorshift64star";
    case FixturePattern::Repeating: return "repeating-byte";
    case FixturePattern::Csprng: return "BCryptGenRandom";
    }
    return "unknown";
}

[[nodiscard]] const char* GetCompressionName(const pbprotocol::CompressionCodec codec) noexcept
{
    switch (codec)
    {
    case pbprotocol::CompressionCodec::Raw: return "RAW";
    case pbprotocol::CompressionCodec::Zstandard: return "zstd";
    }
    return "unknown";
}

[[nodiscard]] const char* GetOuterFecName(const pbprotocol::OuterFecMode mode) noexcept
{
    switch (mode)
    {
    case pbprotocol::OuterFecMode::WirehairV2: return "WirehairV2";
    case pbprotocol::OuterFecMode::DirectRepeat: return "DirectRepeat";
    }
    return "unknown";
}

void GenerateSourceFile(const std::filesystem::path& path, const FixtureDefinition& definition)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(output), "cannot create fixture source file");
    constexpr std::size_t chunkBytes = 1024 * 1024;
    std::vector<std::byte> chunk(chunkBytes);
    std::uint64_t state = 0xD1B54A32D192ED03ULL ^ definition.byteCount;
    std::uint64_t writtenBytes = 0;
    while (writtenBytes < definition.byteCount)
    {
        const std::size_t currentBytes = static_cast<std::size_t>((std::min)(
            definition.byteCount - writtenBytes, static_cast<std::uint64_t>(chunk.size())));
        if (definition.pattern == FixturePattern::Repeating)
        {
            std::fill_n(chunk.begin(), currentBytes, std::byte{0x41});
        }
        else if (definition.pattern == FixturePattern::Csprng)
        {
            RequireNtSuccess(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(chunk.data()),
                static_cast<ULONG>(currentBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                "BCryptGenRandom(fixture)");
        }
        else
        {
            for (std::size_t offset = 0; offset < currentBytes; offset += sizeof(std::uint64_t))
            {
                state ^= state >> 12U;
                state ^= state << 25U;
                state ^= state >> 27U;
                const std::uint64_t randomValue = state * 0x2545F4914F6CDD1DULL;
                const std::size_t availableBytes = (std::min)(sizeof(randomValue), currentBytes - offset);
                for (std::size_t byteIndex = 0; byteIndex < availableBytes; byteIndex++)
                {
                    chunk[offset + byteIndex] = static_cast<std::byte>(randomValue >> (byteIndex * 8U));
                }
            }
        }
        output.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(currentBytes));
        Require(output.good(), "fixture source write failed");
        writtenBytes += currentBytes;
    }
    output.flush();
    Require(output.good(), "fixture source flush failed");
}

template <typename Consumer>
void ReadFileChunks(const std::filesystem::path& path, Consumer&& consumer)
{
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "cannot open file for external digest verification");
    std::vector<std::byte> chunk(1024 * 1024);
    while (true)
    {
        input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize readBytes = input.gcount();
        Require(readBytes >= 0, "external digest read returned a negative byte count");
        if (readBytes > 0)
        {
            consumer(std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(readBytes)));
        }
        if (input.eof())
        {
            break;
        }
        Require(input.good(), "external digest file read failed");
    }
}

[[nodiscard]] ExternalFileEvidence InspectFile(const std::filesystem::path& path)
{
    const std::uintmax_t fileBytes = std::filesystem::file_size(path);
    Require(fileBytes <= (std::numeric_limits<std::uint64_t>::max)(),
        "external file length exceeds uint64");
    ExternalFileEvidence evidence;
    evidence.byteCount = static_cast<std::uint64_t>(fileBytes);
    BCryptAlgorithm algorithm;
    BCryptSha256Hash sha256(algorithm.Get());
    pbprotocol::Blake3Hasher blake3;
    ReadFileChunks(path, [&](const std::span<const std::byte> bytes)
    {
        sha256.Update(bytes);
        blake3.Update(bytes);
    });
    evidence.sha256 = sha256.Finish();
    evidence.blake3 = blake3.Finalize();
    return evidence;
}

[[nodiscard]] bool FilesAreByteExact(const std::filesystem::path& leftPath,
    const std::filesystem::path& rightPath)
{
    if (std::filesystem::file_size(leftPath) != std::filesystem::file_size(rightPath))
    {
        return false;
    }
    std::ifstream left(leftPath, std::ios::binary);
    std::ifstream right(rightPath, std::ios::binary);
    Require(static_cast<bool>(left) && static_cast<bool>(right),
        "cannot open files for byte-exact comparison");
    std::vector<std::byte> leftChunk(1024 * 1024);
    std::vector<std::byte> rightChunk(leftChunk.size());
    while (true)
    {
        left.read(reinterpret_cast<char*>(leftChunk.data()), static_cast<std::streamsize>(leftChunk.size()));
        right.read(reinterpret_cast<char*>(rightChunk.data()), static_cast<std::streamsize>(rightChunk.size()));
        const std::streamsize leftBytes = left.gcount();
        const std::streamsize rightBytes = right.gcount();
        if (leftBytes != rightBytes)
        {
            return false;
        }
        if (leftBytes == 0)
        {
            Require(left.eof() && right.eof(), "byte-exact comparison read failed");
            return true;
        }
        if (!std::equal(leftChunk.begin(), leftChunk.begin() + leftBytes, rightChunk.begin()))
        {
            return false;
        }
    }
}

[[nodiscard]] CaseEvidence RunFixture(const std::filesystem::path& scratchRoot,
    const FixtureDefinition& definition)
{
    const std::filesystem::path caseRoot = scratchRoot / definition.name;
    std::error_code error;
    std::filesystem::remove_all(caseRoot, error);
    Require(!error, "cannot clear the fixture-owned case directory");
    Require(std::filesystem::create_directories(caseRoot / L"output", error) && !error,
        "cannot create the fixture case directories");
    CaseEvidence evidence;
    evidence.definition = definition;
    evidence.sourcePath = caseRoot / L"source.bin";
    GenerateSourceFile(evidence.sourcePath, definition);
    pbapp::ApplicationHeadlessProbeOptions options;
    options.compressionEnabled = definition.compressionEnabled;
    options.compressionLevel = 3;
    options.exerciseFourActiveBusy = definition.exerciseFourActiveBusy;
    const pbapp::RuntimeStatus status = pbapp::ApplicationRuntimeTestAccess::ProbeHeadlessMultiSegmentFile(
        evidence.sourcePath.wstring(), caseRoot / L"sender-state", (caseRoot / L"output").wstring(),
        options, evidence.probe);
    Require(static_cast<bool>(status), std::string("headless application probe failed: ") + status.message);
    evidence.outputPath = evidence.probe.publishedPath;
    evidence.source = InspectFile(evidence.sourcePath);
    evidence.output = InspectFile(evidence.outputPath);
    evidence.byteExact = FilesAreByteExact(evidence.sourcePath, evidence.outputPath);
    Require(evidence.source.byteCount == definition.byteCount &&
        evidence.output.byteCount == definition.byteCount, "external file length mismatch");
    Require(evidence.source.sha256 == evidence.output.sha256, "external SHA-256 mismatch");
    Require(evidence.source.blake3 == evidence.output.blake3, "external BLAKE3 mismatch");
    Require(evidence.source.blake3 == evidence.probe.wholeFileDigest,
        "external source BLAKE3 disagrees with the formal FinalManifest");
    Require(evidence.byteExact, "published file is not byte-exact");
    Require(evidence.probe.authoritativePublish && evidence.probe.sourceStable,
        "application did not report an authoritative stable-source publish");
    if (evidence.probe.segmentCount == 0)
    {
        Require(evidence.probe.preDescriptorOrphanBlocks == 0 &&
            evidence.probe.exactDuplicateSegmentDescriptors == 0,
            "empty fixture reported nonexistent Segment activity");
    }
    else
    {
        Require(evidence.probe.preDescriptorOrphanBlocks == 1 &&
            evidence.probe.exactDuplicateSegmentDescriptors == 1,
            "nonempty fixture did not prove orphan drain and exact descriptor duplication");
    }
    if (evidence.probe.segmentCount > 1)
    {
        Require(evidence.probe.reversedSegmentDescriptors == evidence.probe.segmentCount,
            "multi-Segment fixture did not admit every descriptor in reverse order");
    }
    bool hasZstandard = false;
    for (const pbapp::ApplicationHeadlessSegmentProbeSnapshot& segment : evidence.probe.segments)
    {
        hasZstandard = hasZstandard || segment.compressionCodec == pbprotocol::CompressionCodec::Zstandard;
        if (definition.codecExpectation == CodecExpectation::RawOnly)
        {
            Require(segment.compressionCodec == pbprotocol::CompressionCodec::Raw &&
                segment.rawBytes == segment.encodedBytes, "fixture did not retain the expected RAW encoding");
        }
    }
    if (definition.codecExpectation == CodecExpectation::HasZstandard)
    {
        Require(hasZstandard, "compressible fixture did not select zstd");
    }
    if (definition.exerciseFourActiveBusy)
    {
        Require(evidence.probe.peakReceiverActiveOuterFecDecoderCount == 4 &&
            evidence.probe.deferredResourceBusyCount == 1 &&
            evidence.probe.successfulBusyRetryCount == 1,
            "fixture did not prove the four-active busy/retry transition");
    }
    return evidence;
}

void WriteReport(const std::filesystem::path& reportPath, const std::vector<CaseEvidence>& cases)
{
    Require(!cases.empty(), "CP-A report has no cases");
    std::uint32_t maximumSenderSegments = 0;
    std::uint64_t maximumSenderBytes = 0;
    std::uint64_t maximumReceiverDecoders = 0;
    std::uint64_t maximumReceiverDecoderBytes = 0;
    std::uint64_t maximumOrphanCachedBytes = 0;
    std::uint64_t maximumResumeActiveBytes = 0;
    std::uint64_t maximumResumePendingBytes = 0;
    std::uint64_t maximumResumeResidentBytes = 0;
    for (const CaseEvidence& evidence : cases)
    {
        maximumSenderSegments = (std::max)(maximumSenderSegments,
            evidence.probe.peakSenderResidentEncodedSegmentCount);
        maximumSenderBytes = (std::max)(maximumSenderBytes,
            evidence.probe.peakSenderResidentEncodedSegmentBytes);
        maximumReceiverDecoders = (std::max)(maximumReceiverDecoders,
            evidence.probe.peakReceiverActiveOuterFecDecoderCount);
        maximumReceiverDecoderBytes = (std::max)(maximumReceiverDecoderBytes,
            evidence.probe.peakReceiverReservedOuterFecDecoderBytes);
        maximumOrphanCachedBytes = (std::max)(maximumOrphanCachedBytes,
            evidence.probe.peakReceiverOrphanCachedBytes);
        maximumResumeActiveBytes = (std::max)(maximumResumeActiveBytes,
            evidence.probe.peakReceiverResumeActivePayloadBytes);
        maximumResumePendingBytes = (std::max)(maximumResumePendingBytes,
            evidence.probe.peakReceiverResumePendingPayloadBytes);
        maximumResumeResidentBytes = (std::max)(maximumResumeResidentBytes,
            evidence.probe.peakReceiverResumeResidentPayloadBytes);
    }
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(report), "cannot create CP-A JSON report");
    report << "{\n"
           << "  \"schema\": \"PixelBridge.UnifiedVisualCpAHeadless.1\",\n"
           << "  \"gate\": \"G04/CP-A\",\n"
           << "  \"mode\": \"production-application-no-raster\",\n"
           << "  \"visualChainCovered\": false,\n"
           << "  \"allPublishedByteExact\": true,\n"
           << "  \"workingSetWithinLimits\": true,\n"
           << "  \"externalVerification\": [\"length\", \"SHA-256/CNG\", \"BLAKE3\", \"byte-compare\"],\n"
           << "  \"caseCount\": " << cases.size() << ",\n"
           << "  \"workingSetLimits\": {\"senderEncodedSegments\": 2, \"senderEncodedBytes\": "
           << 2 * pbprotocol::kDefaultSourceSegmentTargetBytes
           << ", \"receiverActiveOuterFecDecoders\": "
           << cases.front().probe.receiverActiveOuterFecDecoderLimit
           << ", \"receiverReservedOuterFecDecoderBytes\": "
           << cases.front().probe.receiverTotalOuterFecDecoderByteLimit
           << ", \"receiverResumeActiveOrPendingPayloadBytes\": "
           << cases.front().probe.receiverResumeByteLimit
           << ", \"receiverResumeResidentPayloadCopyBytes\": "
           << 2 * cases.front().probe.receiverResumeByteLimit << "},\n"
           << "  \"workingSetHighWater\": {\"senderEncodedSegments\": " << maximumSenderSegments
           << ", \"senderEncodedBytes\": " << maximumSenderBytes
           << ", \"receiverActiveOuterFecDecoders\": " << maximumReceiverDecoders
           << ", \"receiverReservedOuterFecDecoderBytes\": " << maximumReceiverDecoderBytes
           << ", \"receiverOrphanCachedBytes\": " << maximumOrphanCachedBytes
           << ", \"receiverResumeActivePayloadBytes\": " << maximumResumeActiveBytes
           << ", \"receiverResumePendingPayloadBytes\": " << maximumResumePendingBytes
           << ", \"receiverResumeResidentPayloadBytes\": " << maximumResumeResidentBytes << "},\n"
           << "  \"cases\": [\n";
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); caseIndex++)
    {
        const CaseEvidence& evidence = cases[caseIndex];
        const pbapp::ApplicationHeadlessProbeSnapshot& probe = evidence.probe;
        report << "    {\n"
               << "      \"name\": \"" << JsonEscape(evidence.definition.name) << "\",\n"
               << "      \"generator\": \"" << GetPatternName(evidence.definition.pattern) << "\",\n"
               << "      \"sourcePath\": \"" << JsonEscape(WideToUtf8(evidence.sourcePath.wstring())) << "\",\n"
               << "      \"outputPath\": \"" << JsonEscape(WideToUtf8(evidence.outputPath.wstring())) << "\",\n"
               << "      \"bytes\": " << evidence.source.byteCount << ",\n"
               << "      \"segmentCount\": " << probe.segmentCount << ",\n"
               << "      \"compressionEnabled\": " << (evidence.definition.compressionEnabled ? "true" : "false") << ",\n"
               << "      \"sourceSha256\": \"" << BytesToHex(evidence.source.sha256) << "\",\n"
               << "      \"outputSha256\": \"" << BytesToHex(evidence.output.sha256) << "\",\n"
               << "      \"sourceBlake3\": \"" << BytesToHex(evidence.source.blake3) << "\",\n"
               << "      \"outputBlake3\": \"" << BytesToHex(evidence.output.blake3) << "\",\n"
               << "      \"byteExact\": " << (evidence.byteExact ? "true" : "false") << ",\n"
               << "      \"processedControlRecords\": " << probe.processedControlRecords << ",\n"
               << "      \"repeatedControlRecords\": " << probe.repeatedControlRecords << ",\n"
               << "      \"reversedSegmentDescriptors\": " << probe.reversedSegmentDescriptors << ",\n"
               << "      \"exactDuplicateSegmentDescriptors\": "
               << probe.exactDuplicateSegmentDescriptors << ",\n"
               << "      \"preDescriptorOrphanBlocks\": " << probe.preDescriptorOrphanBlocks << ",\n"
               << "      \"submittedSystematicBlocks\": " << probe.submittedSystematicBlocks << ",\n"
               << "      \"submittedRepairBlocks\": " << probe.submittedRepairBlocks << ",\n"
               << "      \"intentionallySkippedSystematicBlocks\": " << probe.intentionallySkippedSystematicBlocks << ",\n"
               << "      \"deferredResourceBusyCount\": " << probe.deferredResourceBusyCount << ",\n"
               << "      \"successfulBusyRetryCount\": " << probe.successfulBusyRetryCount << ",\n"
               << "      \"workingSetHighWater\": {\"senderEncodedSegments\": "
               << probe.peakSenderResidentEncodedSegmentCount << ", \"senderEncodedBytes\": "
               << probe.peakSenderResidentEncodedSegmentBytes << ", \"receiverActiveOuterFecDecoders\": "
               << probe.peakReceiverActiveOuterFecDecoderCount << ", \"receiverReservedOuterFecDecoderBytes\": "
               << probe.peakReceiverReservedOuterFecDecoderBytes << ", \"receiverOrphanCachedBytes\": "
               << probe.peakReceiverOrphanCachedBytes << ", \"receiverResumeActivePayloadBytes\": "
               << probe.peakReceiverResumeActivePayloadBytes << ", \"receiverResumePendingPayloadBytes\": "
               << probe.peakReceiverResumePendingPayloadBytes << ", \"receiverResumeResidentPayloadBytes\": "
               << probe.peakReceiverResumeResidentPayloadBytes << "},\n"
               << "      \"segments\": [\n";
        for (std::size_t segmentIndex = 0; segmentIndex < probe.segments.size(); segmentIndex++)
        {
            const pbapp::ApplicationHeadlessSegmentProbeSnapshot& segment = probe.segments[segmentIndex];
            report << "        {\"ordinal\": " << segment.segmentOrdinal
                   << ", \"rawBytes\": " << segment.rawBytes
                   << ", \"encodedBytes\": " << segment.encodedBytes
                   << ", \"compression\": \"" << GetCompressionName(segment.compressionCodec)
                   << "\", \"outerFec\": \"" << GetOuterFecName(segment.outerFecMode)
                   << "\", \"systematicBlockCount\": " << segment.systematicBlockCount
                   << ", \"submittedSystematicBlocks\": " << segment.submittedSystematicBlocks
                   << ", \"submittedRepairBlocks\": " << segment.submittedRepairBlocks
                   << ", \"intentionallySkippedSystematicBlocks\": "
                   << segment.intentionallySkippedSystematicBlocks << "}"
                   << (segmentIndex + 1 == probe.segments.size() ? "\n" : ",\n");
        }
        report << "      ]\n"
               << "    }" << (caseIndex + 1 == cases.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    report.flush();
    Require(report.good(), "CP-A JSON report write failed");
}

} // namespace

int main()
{
    try
    {
        const std::filesystem::path artifactRoot = std::filesystem::path(PB_TEST_SCRATCH_ROOT);
        const std::filesystem::path scratchRoot = artifactRoot / L"g04-cp-a-scratch";
        const std::filesystem::path reportPath = artifactRoot / L"g04-cp-a-headless-report.json";
        std::error_code error;
        std::filesystem::remove_all(scratchRoot, error);
        Require(!error, "cannot clear the CP-A scratch root");
        Require(std::filesystem::create_directories(scratchRoot, error) && !error,
            "cannot create the CP-A scratch root");
        const std::array<FixtureDefinition, 9> fixtures{
            FixtureDefinition{"empty-0b", 0, FixturePattern::Deterministic, false, false, CodecExpectation::RawOnly},
            FixtureDefinition{"single-1b", 1, FixturePattern::Deterministic, false, false, CodecExpectation::RawOnly},
            FixtureDefinition{"boundary-8m-minus-1", 8 * mebibyte - 1, FixturePattern::Deterministic, false, false, CodecExpectation::RawOnly},
            FixtureDefinition{"boundary-8m", 8 * mebibyte, FixturePattern::Deterministic, false, false, CodecExpectation::RawOnly},
            FixtureDefinition{"boundary-8m-plus-1", 8 * mebibyte + 1, FixturePattern::Deterministic, false, false, CodecExpectation::RawOnly},
            FixtureDefinition{"large-csprng-24m", 24 * mebibyte, FixturePattern::Csprng, true, false, CodecExpectation::RawOnly},
            FixtureDefinition{"small-compressible", 64 * 1024, FixturePattern::Repeating, true, false, CodecExpectation::HasZstandard},
            FixtureDefinition{"small-incompressible", 64 * 1024, FixturePattern::Csprng, true, false, CodecExpectation::RawOnly},
            FixtureDefinition{"five-segment-busy-retry", 32 * mebibyte + 1, FixturePattern::Deterministic, false, true, CodecExpectation::RawOnly}};
        std::vector<CaseEvidence> cases;
        cases.reserve(fixtures.size());
        for (const FixtureDefinition& fixture : fixtures)
        {
            std::cout << "[G04] running " << fixture.name << " (" << fixture.byteCount << " bytes)" << std::endl;
            cases.push_back(RunFixture(scratchRoot, fixture));
            std::cout << "[G04] PASS " << fixture.name << " sha256="
                      << BytesToHex(cases.back().source.sha256) << std::endl;
        }
        WriteReport(reportPath, cases);
        std::cout << "[G04] PASS all " << cases.size() << " cases; report="
                  << WideToUtf8(std::filesystem::absolute(reportPath).wstring()) << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[G04] FAIL: " << exception.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[G04] FAIL: unknown exception" << std::endl;
        return 1;
    }
}
