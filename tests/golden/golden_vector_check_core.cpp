#include "golden_vector_check_core.h"

#include "pbgolden/golden_vector_source.h"
#include "pbprotocol/blake3_digest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pbgoldenchk {

namespace {

constexpr std::size_t kReferenceFrameBgraBytes = 8294400;

[[nodiscard]] std::string ToHexLowerBytes(
    const std::span<const std::byte> bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2u);
    for (const std::byte value : bytes)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        result.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
        result.push_back(kHexDigits[byteValue & 0xFu]);
    }
    return result;
}

[[nodiscard]] CheckReport MakeReport(
    const std::string_view subject, const std::string_view category,
    const CheckKind kind)
{
    CheckReport report;
    report.subject = subject;
    report.category = category;
    report.kind = kind;
    return report;
}

[[nodiscard]] std::string OffsetHex(std::size_t value)
{
    if (value == 0)
    {
        return "0";
    }
    std::string hex;
    while (value > 0)
    {
        hex.insert(hex.begin(), static_cast<char>("0123456789abcdef"[value & 0xFu]));
        value >>= 4;
    }
    return hex;
}

[[nodiscard]] std::string ByteText(const std::byte value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    const auto byteValue = std::to_integer<std::uint8_t>(value);
    std::string text = "0x";
    text.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
    text.push_back(kHexDigits[byteValue & 0xFu]);
    return text;
}

void AppendByteDifference(std::string& line, const CheckReport& report)
{
    if (!report.hasByteDifference)
    {
        return;
    }
    line += " byte_offset=0x" + OffsetHex(report.firstDifferingOffset) +
        " (" + std::to_string(report.firstDifferingOffset) + ") expected=" +
        (report.expectedEof ? "<eof>" : ByteText(report.expectedByte)) +
        " actual=" + (report.actualEof ? "<eof>" : ByteText(report.actualByte)) +
        " (" + std::to_string(report.differingBytes) + "/" +
        std::to_string(report.comparedBytes) + " compared bytes differ)";
}

// Frame payload selection by registry id (same mapping the PBVectorGen
// dump-manifest path uses; the frame pins are bound to these payloads).
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

} // namespace

const char* ToString(const CheckKind kind) noexcept
{
    switch (kind)
    {
    case CheckKind::Pass:
        return "Pass";
    case CheckKind::FileUnreadable:
        return "FileUnreadable";
    case CheckKind::FileDigestMismatch:
        return "FileDigestMismatch";
    case CheckKind::FileAndVectorMismatch:
        return "FileAndVectorMismatch";
    case CheckKind::RecomputeFailed:
        return "RecomputeFailed";
    case CheckKind::VectorMismatch:
        return "VectorMismatch";
    case CheckKind::FrameRasterMismatch:
        return "FrameRasterMismatch";
    case CheckKind::FrameDigestMismatch:
        return "FrameDigestMismatch";
    }
    return "Unknown";
}

std::string ToHexLower(
    const std::array<std::byte, 32>& digest)
{
    return ToHexLowerBytes(std::span<const std::byte>(digest));
}

CheckReport CheckFileDigest(
    const pbgolden::GoldenVectorDescriptor& descriptor,
    const std::span<const std::byte> fileBytes)
{
    const auto fileDigest =
        pbprotocol::ComputeBlake3Digest(fileBytes);
    CheckReport report = MakeReport(
        std::string(descriptor.name), std::string(descriptor.category),
        fileDigest == descriptor.blake3 ? CheckKind::Pass
                                        : CheckKind::FileDigestMismatch);
    report.fileDigest = fileDigest;
    report.pinnedDigest = descriptor.blake3;
    report.sizeFile = fileBytes.size();
    return report;
}

CheckReport CompareVectorBytes(
    const pbgolden::GoldenVectorDescriptor& descriptor,
    const std::span<const std::byte> fileBytes,
    const std::span<const std::byte> recomputedBytes)
{
    const auto fileDigest =
        pbprotocol::ComputeBlake3Digest(fileBytes);
    const auto recomputedDigest =
        pbprotocol::ComputeBlake3Digest(recomputedBytes);
    CheckReport report = MakeReport(
        std::string(descriptor.name), std::string(descriptor.category),
        CheckKind::VectorMismatch);
    report.fileDigest = fileDigest;
    report.recomputedDigest = recomputedDigest;
    report.pinnedDigest = descriptor.blake3;
    report.sizeFile = fileBytes.size();
    report.sizeRecomputed = recomputedBytes.size();

    report.comparedBytes = std::min(fileBytes.size(), recomputedBytes.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < report.comparedBytes; i++)
    {
        if (fileBytes[i] != recomputedBytes[i])
        {
            if (differing == 0)
            {
                report.firstDifferingOffset = i;
                report.expectedByte = fileBytes[i];
                report.actualByte = recomputedBytes[i];
                report.hasByteDifference = true;
            }
            differing++;
        }
    }
    if (fileBytes.size() != recomputedBytes.size())
    {
        if (!report.hasByteDifference)
        {
            report.firstDifferingOffset = report.comparedBytes;
            report.hasByteDifference = true;
            report.expectedEof = fileBytes.size() == report.comparedBytes;
            report.actualEof = recomputedBytes.size() == report.comparedBytes;
            if (!report.expectedEof)
            {
                report.expectedByte = fileBytes[report.comparedBytes];
            }
            if (!report.actualEof)
            {
                report.actualByte = recomputedBytes[report.comparedBytes];
            }
        }
        differing += fileBytes.size() > recomputedBytes.size()
            ? fileBytes.size() - recomputedBytes.size()
            : recomputedBytes.size() - fileBytes.size();
    }
    if (differing == 0)
    {
        report.kind = CheckKind::Pass;
        return report;
    }
    report.differingBytes = differing;
    return report;
}

CheckReport CheckVectorFile(
    const std::filesystem::path& goldenRoot,
    const pbgolden::GoldenVectorDescriptor& descriptor)
{
    const std::filesystem::path file =
        goldenRoot / std::string(descriptor.category) /
        (std::string(descriptor.name) + ".bin");
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        CheckReport report = MakeReport(
            std::string(descriptor.name),
            std::string(descriptor.category), CheckKind::FileUnreadable);
        report.detail = "file missing or unreadable: " + file.generic_string();
        return report;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > 65571u)
    {
        CheckReport report = MakeReport(
            std::string(descriptor.name), std::string(descriptor.category),
            CheckKind::FileUnreadable);
        report.detail = "invalid or oversized file: " + file.generic_string();
        return report;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> fileBytes(static_cast<std::size_t>(size));
    if (!fileBytes.empty())
    {
        stream.read(reinterpret_cast<char*>(fileBytes.data()),
            static_cast<std::streamsize>(fileBytes.size()));
    }
    if (!stream)
    {
        CheckReport report = MakeReport(
            std::string(descriptor.name),
            std::string(descriptor.category), CheckKind::FileUnreadable);
        report.detail = "io error reading: " + file.generic_string();
        return report;
    }

    // Compute all three states before attribution: file, recompute and pin.
    // This avoids calling an arbitrary drifting byte stream "expected".
    const auto digestReport = CheckFileDigest(
        descriptor, std::span<const std::byte>(fileBytes));
    const auto outcome = pbgolden::RecomputeGoldenVector(descriptor.name);
    if (outcome.status != pbgolden::RecomputeStatus::Ok)
    {
        CheckReport report = MakeReport(
            std::string(descriptor.name),
            std::string(descriptor.category), CheckKind::RecomputeFailed);
        report.detail = outcome.detail;
        report.fileDigest = digestReport.fileDigest;
        report.pinnedDigest = descriptor.blake3;
        report.sizeFile = fileBytes.size();
        return report;
    }
    const auto recomputedDigest = pbprotocol::ComputeBlake3Digest(outcome.bytes);
    const bool fileMatchesPin = digestReport.fileDigest == descriptor.blake3;
    const bool recomputeMatchesPin = recomputedDigest == descriptor.blake3;
    if (fileMatchesPin)
    {
        auto report = CompareVectorBytes(descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(outcome.bytes));
        if (!recomputeMatchesPin || report.kind != CheckKind::Pass)
        {
            report.kind = CheckKind::VectorMismatch;
        }
        return report;
    }
    if (recomputeMatchesPin)
    {
        auto report = CompareVectorBytes(descriptor,
            std::span<const std::byte>(outcome.bytes),
            std::span<const std::byte>(fileBytes));
        report.kind = CheckKind::FileDigestMismatch;
        report.fileDigest = digestReport.fileDigest;
        report.recomputedDigest = recomputedDigest;
        report.pinnedDigest = descriptor.blake3;
        report.sizeFile = fileBytes.size();
        report.sizeRecomputed = outcome.bytes.size();
        return report;
    }
    // Both streams drifted from the pin at once: committed-file corruption AND
    // implementation drift simultaneously (any single change surfaces first as
    // VectorMismatch or FileDigestMismatch against the pristine other side), so
    // this attribution is unreachable end to end in a healthy tree and its exact
    // report-line format is pinned by the synthetic-report test in
    // test_golden_vector_check.cpp. No unpinned stream may be labeled
    // authoritative: all three digests plus the byte_offset=not-authoritative
    // sentinel are reported instead.
    CheckReport report = MakeReport(std::string(descriptor.name),
        std::string(descriptor.category), CheckKind::FileAndVectorMismatch);
    report.fileDigest = digestReport.fileDigest;
    report.recomputedDigest = recomputedDigest;
    report.pinnedDigest = descriptor.blake3;
    report.sizeFile = fileBytes.size();
    report.sizeRecomputed = outcome.bytes.size();
    return report;
}

CheckReport CheckFrameVector(const pbgolden::FrameVectorPin& pin)
{
    try
    {
        const auto payload = MakeFramePayload(pin.id);
        const auto result = pbgolden::ComputeFrameDigests(payload);
        if (!result.success)
        {
            CheckReport report = MakeReport(
                pin.name, "frame", CheckKind::RecomputeFailed);
            report.detail = result.detail;
            return report;
        }
        CheckReport report = MakeReport(
            pin.name, "frame", CheckKind::Pass);
        if (!result.rawOracleMatches)
        {
            report.kind = CheckKind::FrameRasterMismatch;
            report.detail = "space=raw-pbrw";
            report.sizeFile = result.rawExpectedSize;
            report.sizeRecomputed = result.rawActualSize;
            report.firstDifferingOffset = result.rawFirstDifferingOffset;
            report.expectedByte = result.rawExpectedByte;
            report.actualByte = result.rawActualByte;
            report.expectedEof = result.rawExpectedEof;
            report.actualEof = result.rawActualEof;
            report.hasByteDifference = true;
            report.differingBytes = result.rawDifferingBytes;
            report.comparedBytes = std::min(result.rawExpectedSize, result.rawActualSize);
            return report;
        }
        if (!result.pngPixelsMatch)
        {
            report.kind = CheckKind::FrameRasterMismatch;
            report.detail = "space=decoded-png-bgra";
            report.sizeFile = kReferenceFrameBgraBytes;
            report.sizeRecomputed = kReferenceFrameBgraBytes;
            report.firstDifferingOffset = result.pngPixelFirstDifferingOffset;
            report.expectedByte = result.pngPixelExpectedByte;
            report.actualByte = result.pngPixelActualByte;
            report.hasByteDifference = true;
            report.differingBytes = result.pngPixelDifferingBytes;
            report.comparedBytes = kReferenceFrameBgraBytes;
            return report;
        }
        if (result.rawBlake3 != pin.rawBlake3)
        {
            report.kind = CheckKind::FrameDigestMismatch;
            report.detail = "channel=raw";
            report.fileDigest = result.rawBlake3;
            report.pinnedDigest = pin.rawBlake3;
            return report;
        }
        if (result.pngBlake3 != pin.pngBlake3)
        {
            report.kind = CheckKind::FrameDigestMismatch;
            report.detail = "channel=png";
            report.fileDigest = result.pngBlake3;
            report.pinnedDigest = pin.pngBlake3;
            return report;
        }
        report.fileDigest = result.rawBlake3;
        report.recomputedDigest = result.pngBlake3;
        return report;
    }
    catch (const std::exception& exception)
    {
        CheckReport report = MakeReport(
            pin.name, "frame", CheckKind::RecomputeFailed);
        report.detail = std::string("frame vector exception: ") + exception.what();
        return report;
    }
    catch (...)
    {
        CheckReport report = MakeReport(
            pin.name, "frame", CheckKind::RecomputeFailed);
        report.detail = "frame vector exception: unknown exception";
        return report;
    }
}

CheckReport CheckManifestVector()
{
    const auto computed = pbgolden::ComputeManifestDigest();
    const auto& pinned = pbgolden::GetManifestDigestPin();
    CheckReport report = MakeReport(
        "manifest", "frame",
        computed == pinned ? CheckKind::Pass
                           : CheckKind::VectorMismatch);
    report.fileDigest = computed;
    report.pinnedDigest = pinned;
    return report;
}

std::string FormatReportLine(const CheckReport& report)
{
    const std::string verb =
        report.kind == CheckKind::Pass ? "PASS" : "FAIL";
    std::string line = "[GOLDEN] " + verb + " ";
    const bool isFrame = report.category == "frame";
    if (!isFrame)
    {
        line += "vector=" + report.subject + " category=" + report.category;
    }
    else if (report.subject == "manifest")
    {
        line += "manifest";
    }
    else
    {
        line += "frame=" + report.subject;
    }
    if (report.kind == CheckKind::Pass)
    {
        if (!isFrame)
        {
            line += " size=" + std::to_string(report.sizeFile) +
                " blake3=" + ToHexLower(report.fileDigest);
        }
        else if (report.subject == "manifest")
        {
            line += " blake3=" + ToHexLower(report.fileDigest);
        }
        else
        {
            line += " raw=blake3:" + ToHexLower(report.fileDigest) +
                " png=blake3:" + ToHexLower(report.recomputedDigest);
        }
        return line;
    }
    line += " kind=" + std::string(ToString(report.kind));
    switch (report.kind)
    {
    case CheckKind::FileUnreadable:
        line += " detail=" + report.detail;
        break;
    case CheckKind::FileDigestMismatch:
        line += " size=" + std::to_string(report.sizeFile) +
            " file=blake3:" + ToHexLower(report.fileDigest) +
            " pinned=blake3:" + ToHexLower(report.pinnedDigest);
        AppendByteDifference(line, report);
        break;
    case CheckKind::FileAndVectorMismatch:
        line += " size_file=" + std::to_string(report.sizeFile) +
            " size_recomputed=" + std::to_string(report.sizeRecomputed) +
            " file=blake3:" + ToHexLower(report.fileDigest) +
            " recomputed=blake3:" + ToHexLower(report.recomputedDigest) +
            " pinned=blake3:" + ToHexLower(report.pinnedDigest) +
            " byte_offset=not-authoritative expected=pinned-digest actual=file-and-recomputed";
        break;
    case CheckKind::RecomputeFailed:
        line += " detail=" + report.detail;
        break;
    case CheckKind::VectorMismatch:
        if (report.subject == "manifest")
        {
            line += " computed=blake3:" + ToHexLower(report.fileDigest) +
                " pinned=blake3:" + ToHexLower(report.pinnedDigest);
            break;
        }
        line += " size_file=" + std::to_string(report.sizeFile) +
            " size_recomputed=" + std::to_string(report.sizeRecomputed);
        AppendByteDifference(line, report);
        line += " file=blake3:" + ToHexLower(report.fileDigest) +
            " recomputed=blake3:" + ToHexLower(report.recomputedDigest) +
            " pinned=blake3:" + ToHexLower(report.pinnedDigest);
        break;
    case CheckKind::FrameRasterMismatch:
        line += " " + report.detail +
            " size_expected=" + std::to_string(report.sizeFile) +
            " size_actual=" + std::to_string(report.sizeRecomputed);
        AppendByteDifference(line, report);
        break;
    case CheckKind::FrameDigestMismatch:
        line += " " + report.detail +
            " byte_offset=not-applicable expected=blake3:" + ToHexLower(report.pinnedDigest) +
            " actual=blake3:" + ToHexLower(report.fileDigest);
        break;
    case CheckKind::Pass:
        break;
    }
    return line;
}

int RunFullCheck(
    const std::filesystem::path& goldenRoot, std::ostream& out)
{
    int failureCount = 0;
    for (const auto& descriptor : pbgolden::GetGoldenVectorRegistry())
    {
        const auto report = CheckVectorFile(goldenRoot, descriptor);
        if (report.kind != CheckKind::Pass)
        {
            failureCount++;
        }
        out << FormatReportLine(report) << "\n";
    }
    for (const auto& pin : pbgolden::GetFrameVectorRegistry())
    {
        const auto report = CheckFrameVector(pin);
        if (report.kind != CheckKind::Pass)
        {
            failureCount++;
        }
        out << FormatReportLine(report) << "\n";
    }
    out << "[GOLDEN] total="
        << (pbgolden::GetGoldenVectorRegistry().size() +
            pbgolden::GetFrameVectorRegistry().size())
        << " failures=" << failureCount << "\n";
    return failureCount;
}

} // namespace pbgoldenchk
