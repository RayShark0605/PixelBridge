#include "pbdesktoplevels/reference_channel.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <new>
#include <ostream>
#include <string_view>
#include <utility>

namespace pbdesktoplevels
{
namespace
{
using pbmodulation::ModulationErrorCode;
using pbmodulation::ModulationStatus;
using pbmodulation::ModulationResult;

struct DiagnosticBinding
{
    std::uint64_t profileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t codewords = 0;
    std::uint32_t paddingBytes = 0;
    std::string_view payloadDomain{};
};

constexpr DiagnosticBinding desktop2Binding{pbmodulation::kDesktopLevels2ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
    86688, 42, 1638, "PB-DesktopLevels-X1-Data"};
constexpr DiagnosticBinding desktop4Binding{pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
    21672, 10, 1422, "PB-DesktopLevels-X1-Data"};
constexpr DiagnosticBinding shapeChromaBinding{pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
    pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords, pbmodulation::kShapeChromaPaddingBytes,
    "PB-ShapeChroma-1-Data"};
constexpr DiagnosticBinding remoteVisualBinding{pbmodulation::kRemoteVisualProfileId, pbmodulation::kRemoteVisualLayoutVersion,
    pbmodulation::kRemoteVisualDataBytes, pbmodulation::kRemoteVisualCodewords, pbmodulation::kRemoteVisualPaddingBytes,
    "PB-RemoteVisual-X2-Data"};
static_assert(desktop2Binding.codewords * kCodewordBytes + desktop2Binding.paddingBytes == desktop2Binding.dataBytes);
static_assert(desktop4Binding.codewords * kCodewordBytes + desktop4Binding.paddingBytes == desktop4Binding.dataBytes);
static_assert(shapeChromaBinding.codewords * kCodewordBytes + shapeChromaBinding.paddingBytes == shapeChromaBinding.dataBytes);
static_assert(remoteVisualBinding.codewords * kCodewordBytes + remoteVisualBinding.paddingBytes == remoteVisualBinding.dataBytes);

bool Overlap(const std::span<const std::byte> first, const std::span<const std::byte> second) noexcept
{
    if (first.empty() || second.empty())
    {
        return false;
    }
    const auto firstAddress = reinterpret_cast<std::uintptr_t>(first.data());
    const auto secondAddress = reinterpret_cast<std::uintptr_t>(second.data());
    // Reject wrapped declared spans before forming an out-of-range pointer.
    if (first.size() > std::numeric_limits<std::uintptr_t>::max() - firstAddress ||
        second.size() > std::numeric_limits<std::uintptr_t>::max() - secondAddress)
    {
        return true;
    }
    return firstAddress <= secondAddress ? secondAddress - firstAddress < first.size() : firstAddress - secondAddress < second.size();
}

const DiagnosticBinding* ParseBinding(const std::span<const std::byte> bytes, pbprotocol::BootstrapRecord& record) noexcept
{
    if (bytes.size() != pbmodulation::kLocalDesktopBootstrapRecordBytes || bytes.data() == nullptr)
    {
        return nullptr;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bytes);
    if (!parsed)
    {
        return nullptr;
    }
    record = parsed.Value();
    if (record.visualProfileId == desktop2Binding.profileId && record.visualLayoutVersion == desktop2Binding.layoutVersion)
    {
        return &desktop2Binding;
    }
    if (record.visualProfileId == desktop4Binding.profileId && record.visualLayoutVersion == desktop4Binding.layoutVersion)
    {
        return &desktop4Binding;
    }
    if (record.visualProfileId == shapeChromaBinding.profileId && record.visualLayoutVersion == shapeChromaBinding.layoutVersion)
    {
        return &shapeChromaBinding;
    }
    return record.visualProfileId == remoteVisualBinding.profileId && record.visualLayoutVersion == remoteVisualBinding.layoutVersion ?
        &remoteVisualBinding : nullptr;
}

void GeneratePayload(const std::string_view domain, const std::span<const std::byte> bootstrapRecord, const std::uint32_t slot,
                      const std::span<std::byte, kPayloadBytes> payload) noexcept
{
    for (std::size_t offset = 0; offset < payload.size(); offset += 32)
    {
        std::array<std::byte, 8> suffix{};
        const auto chunk = static_cast<std::uint32_t>(offset / 32);
        for (std::size_t index = 0; index < 4; index++)
        {
            suffix[index] = static_cast<std::byte>((slot >> (index * 8)) & 255);
            suffix[index + 4] = static_cast<std::byte>((chunk >> (index * 8)) & 255);
        }
        pbprotocol::Blake3Hasher hasher;
        hasher.Update(std::as_bytes(std::span(domain.data(), domain.size())));
        hasher.Update(bootstrapRecord);
        hasher.Update(suffix);
        const auto digest = hasher.Finalize();
        std::copy_n(digest.begin(), std::min(digest.size(), payload.size() - offset), payload.begin() + offset);
    }
}

bool GenerateInto(const std::span<const std::byte> bootstrapRecord, const pbprotocol::BootstrapRecord& record,
                   const DiagnosticBinding& profile, const std::span<std::byte> data) noexcept
{
    std::array<std::byte, kPayloadBytes> payload{};
    std::array<std::byte, kInfoBytes> transport{};
    std::array<std::byte, kInfoBytes> information{};
    for (std::uint32_t slot = 0; slot < profile.codewords; slot++)
    {
        GeneratePayload(profile.payloadDomain, bootstrapRecord, slot, payload);
        pbprotocol::TransportBlockHeader header;
        header.sessionTag = record.sessionTag;
        header.segmentOrdinal = record.frameSequence;
        header.outerBlockId = slot;
        header.payloadBytes = static_cast<std::uint16_t>(kPayloadBytes);
        if (!pbprotocol::SerializeTransportBlock(header, payload, transport) ||
            !pbprotocol::FrameTransportBlockIntoInfoBlock(transport, information.size(), information) ||
            !pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, data.subspan(slot * kCodewordBytes, kCodewordBytes)))
        {
            return false;
        }
    }
    std::fill(data.begin() + profile.codewords * kCodewordBytes, data.end(), std::byte{0});
    return true;
}

enum class RemoteControlExtractResult : std::uint8_t
{
    Invalid,
    Accepted,
    IdentityMismatch
};

RemoteControlExtractResult ExtractRemoteControl(const std::span<const std::byte> information,
    const pbprotocol::SessionTag bootstrapSessionTag,
    AcceptedRemoteControlBlock& output) noexcept
{
    if (information.size() != kInfoBytes || information.data() == nullptr ||
        !std::ranges::all_of(information.subspan(pbmodulation::kReferenceControlWindowBytes),
            [](const std::byte value) { return value == std::byte{0}; }))
    {
        return RemoteControlExtractResult::Invalid;
    }
    const auto window = information.first(pbmodulation::kReferenceControlWindowBytes);
    AcceptedRemoteControlBlock accepted;
    std::uint32_t acceptedCandidates = 0;
    bool identityMismatch = false;
    for (std::size_t byteCount = 1; byteCount <= window.size(); byteCount++)
    {
        if (!std::ranges::all_of(window.subspan(byteCount), [](const std::byte value) { return value == std::byte{0}; }))
        {
            continue;
        }
        const auto candidate = window.first(byteCount);
        const auto record = pbprotocol::ParseControlRecord(candidate);
        if (record && record.Value().sessionTag == bootstrapSessionTag)
        {
            accepted.kind = AcceptedRemoteControlKind::Record;
            accepted.byteCount = static_cast<std::uint32_t>(byteCount);
            acceptedCandidates++;
        }
        else if (record)
        {
            identityMismatch = true;
        }
    }
    if (acceptedCandidates != 1)
    {
        return acceptedCandidates == 0 && identityMismatch ? RemoteControlExtractResult::IdentityMismatch :
            RemoteControlExtractResult::Invalid;
    }
    std::copy_n(window.begin(), accepted.byteCount, accepted.bytes.begin());
    output = accepted;
    return RemoteControlExtractResult::Accepted;
}

void WriteNumber(std::ostream& output, const double value)
{
    if (std::isfinite(value))
    {
        output << std::setprecision(17) << value;
    }
    else
    {
        output << "null";
    }
}

void WriteRatio(std::ostream& output, const std::uint64_t numerator, const std::uint64_t denominator)
{
    if (denominator == 0)
    {
        output << "null";
    }
    else
    {
        WriteNumber(output, static_cast<double>(numerator) / static_cast<double>(denominator));
    }
}

template <std::size_t Rows, std::size_t Columns>
void WriteMatrix(std::ostream& output, const std::array<std::array<double, Columns>, Rows>& values)
{
    output << '[';
    for (std::size_t row = 0; row < Rows; row++)
    {
        if (row != 0)
        {
            output << ',';
        }
        output << '[';
        for (std::size_t column = 0; column < Columns; column++)
        {
            if (column != 0)
            {
                output << ',';
            }
            WriteNumber(output, values[row][column]);
        }
        output << ']';
    }
    output << ']';
}

void WriteMargin(std::ostream& output, const pbmodulation::DesktopLevelsMargin& margin)
{
    output << "{\"samples\":" << margin.samples << ",\"bins\":4096,\"quantileResolution\":";
    WriteNumber(output, 1.0 / 4095);
    output << ",\"min\":";
    if (margin.samples == 0)
    {
        output << "null,\"P50\":null,\"P01\":null,\"P001\":null}";
        return;
    }
    WriteNumber(output, margin.minimum);
    output << ",\"P50\":";
    WriteNumber(output, margin.p50);
    output << ",\"P01\":";
    WriteNumber(output, margin.p01);
    output << ",\"P001\":";
    WriteNumber(output, margin.p001);
    output << '}';
}
} // namespace

ModulationStatus GenerateDiagnosticData(const std::span<const std::byte> bootstrapRecord, const std::span<std::byte> logicalData) noexcept
{
    pbprotocol::BootstrapRecord record;
    const auto* const profile = ParseBinding(bootstrapRecord, record);
    if (profile == nullptr || logicalData.size() != profile->dataBytes || logicalData.data() == nullptr || Overlap(bootstrapRecord, logicalData))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> scratch{};
    const auto data = std::span(scratch).first(profile->dataBytes);
    if (!GenerateInto(bootstrapRecord, record, *profile, data))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InternalInvariantViolation, 0);
    }
    std::copy(data.begin(), data.end(), logicalData.begin());
    return ModulationStatus::Success();
}

void AdaptFiniteSoftMetrics(const std::span<const float> metrics, const std::span<std::int16_t> output) noexcept
{
    for (std::size_t index = 0; index < metrics.size(); index++)
    {
        const double scaled = std::clamp(static_cast<double>(metrics[index]) * kSoftMetricScale, -32767.0, 32767.0);
        output[index] = static_cast<std::int16_t>(std::round(scaled));
    }
}

bool AdaptSoftMetrics(const std::span<const float> metrics, const std::span<std::int16_t> output) noexcept
{
    const auto Finite = [](const float metric)
    {
        return std::isfinite(metric);
    };
    if (metrics.empty() || metrics.size() > pbmodulation::kDesktopLevelsMaximumBits || metrics.size() != output.size() ||
        metrics.data() == nullptr || output.data() == nullptr || Overlap(std::as_bytes(metrics), std::as_bytes(output)) ||
        !std::ranges::all_of(metrics, Finite))
    {
        return false;
    }
    AdaptFiniteSoftMetrics(metrics, output);
    return true;
}

bool FrameEvaluation::IsVerified() const noexcept
{
    return evaluated && paddingValid && codewords != 0 && acceptedTransportBlocks == codewords && fecFailures == 0 &&
        crcFailures == 0 && identityFailures == 0 && falseAcceptedCodewords == 0;
}

struct ReferenceChannel::Implementation
{
    pbmodulation::DesktopLevelsWorkspace modulation;
    pbmodulation::ShapeChromaWorkspace shapeChroma;
    pbmodulation::RemoteVisualWorkspace remoteVisual;
    pbinnerfec::QcLdpcDecoder decoder;
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> hard{};
    std::array<float, pbmodulation::kDesktopLevelsMaximumBits> soft{};
    std::array<std::byte, pbmodulation::kDesktopLevelsMaximumDataBytes> expected{};
    std::array<std::byte, kCodewordBytes * kMaximumCodewords> recovered{};
    std::array<bool, kMaximumCodewords> crcValid{};
    std::array<std::int16_t, kCodewordBits> llr{};
    std::array<AcceptedTransportBlock, kMaximumCodewords> accepted{};
    std::size_t acceptedCount = 0;
    std::array<AcceptedRemoteControlBlock, 1> acceptedRemoteControl{};
    std::size_t acceptedRemoteControlCount = 0;
    bool histogramValid = false;
    bool shapeHistogramValid = false;
    bool remoteVisualHistogramValid = false;
};

ReferenceChannel::ReferenceChannel() noexcept = default;
ReferenceChannel::ReferenceChannel(ReferenceChannel&&) noexcept = default;
ReferenceChannel& ReferenceChannel::operator=(ReferenceChannel&&) noexcept = default;
ReferenceChannel::~ReferenceChannel() = default;

ModulationResult<ReferenceChannel> ReferenceChannel::Create(const std::uint64_t maximumBytes) noexcept
{
    // 1 MiB bounds the existing Robust decoder; another MiB covers bounded
    // application state. Reject before allocation, including tiny budgets.
    if (maximumBytes < kProcessingReservationBytes ||
        sizeof(Implementation) + pbmodulation::DesktopLevelsWorkspace::RequiredBytes() + pbmodulation::ShapeChromaWorkspace::RequiredBytes() +
            pbmodulation::RemoteVisualWorkspace::RequiredBytes() +
            2 * 1024 * 1024 > kProcessingReservationBytes)
    {
        return ModulationResult<ReferenceChannel>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    auto modulation = pbmodulation::DesktopLevelsWorkspace::Create(kProcessingReservationBytes);
    auto shapeChroma = pbmodulation::ShapeChromaWorkspace::Create(kProcessingReservationBytes);
    auto remoteVisual = pbmodulation::RemoteVisualWorkspace::Create(kProcessingReservationBytes);
    auto decoder = pbinnerfec::QcLdpcDecoder::Create(pbinnerfec::kInnerFecProfileIdRobust);
    if (!modulation || !shapeChroma || !remoteVisual || !decoder)
    {
        return ModulationResult<ReferenceChannel>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
    ReferenceChannel result;
    result.implementation_.reset(new (std::nothrow) Implementation);
    if (!result.implementation_)
    {
        return ModulationResult<ReferenceChannel>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
    result.implementation_->modulation = std::move(modulation).Value();
    result.implementation_->shapeChroma = std::move(shapeChroma).Value();
    result.implementation_->remoteVisual = std::move(remoteVisual).Value();
    result.implementation_->decoder = std::move(decoder).Value();
    return ModulationResult<ReferenceChannel>::Success(std::move(result));
}

ReferenceObservation ReferenceChannel::Decode(const pbmodulation::LumaView& view, const pbmodulation::DesktopLevelsDecodePolicy& policy) noexcept
{
    ReferenceObservation result;
    if (!implementation_)
    {
        result.modulation.erasure = pbmodulation::DesktopLevelsErasure::WorkspaceUnavailable;
        return result;
    }
    auto& state = *implementation_;
    state.histogramValid = false;
    state.shapeHistogramValid = false;
    state.remoteVisualHistogramValid = false;
    state.acceptedCount = 0;
    state.acceptedRemoteControlCount = 0;
    result.modulation = pbmodulation::DecodeDesktopLevelsFrame(view, state.modulation, state.hard, state.soft, policy);
    if (result.modulation.IsAccepted())
    {
        const auto bytes = result.modulation.dataBytes;
        result.evaluation = EvaluateCodewords(result.modulation.bootstrap.canonical44, std::span(state.hard).first(bytes), std::span(state.soft).first(bytes * 8ULL));
        state.histogramValid = result.evaluation.evaluated;
    }
    return result;
}

ShapeChromaReferenceObservation ReferenceChannel::DecodeShapeChroma(const pbmodulation::LumaView& view,
    const pbmodulation::ShapeChromaDecodePolicy& policy) noexcept
{
    ShapeChromaReferenceObservation result;
    if (!implementation_)
    {
        result.modulation.erasure = pbmodulation::ShapeChromaErasure::WorkspaceUnavailable;
        return result;
    }
    auto& state = *implementation_;
    state.histogramValid = false;
    state.shapeHistogramValid = false;
    state.remoteVisualHistogramValid = false;
    state.acceptedCount = 0;
    state.acceptedRemoteControlCount = 0;
    result.modulation = pbmodulation::DecodeShapeChromaFrame(view, state.shapeChroma, state.hard, state.soft, policy);
    if (result.modulation.IsAccepted())
    {
        result.evaluation = EvaluateCodewords(result.modulation.bootstrap.canonical44,
            std::span(state.hard).first(result.modulation.dataBytes),
            std::span(state.soft).first(static_cast<std::size_t>(result.modulation.dataBytes) * 8));
        state.shapeHistogramValid = result.evaluation.evaluated;
    }
    return result;
}

RemoteVisualReferenceObservation ReferenceChannel::DecodeRemoteVisual(const pbmodulation::LumaView& view,
    const pbmodulation::RemoteVisualDecodePolicy& policy) noexcept
{
    RemoteVisualReferenceObservation result;
    if (!implementation_)
    {
        result.modulation.erasure = pbmodulation::RemoteVisualErasure::WorkspaceUnavailable;
        return result;
    }
    auto& state = *implementation_;
    state.histogramValid = false;
    state.shapeHistogramValid = false;
    state.remoteVisualHistogramValid = false;
    state.acceptedCount = 0;
    state.acceptedRemoteControlCount = 0;
    result.modulation = pbmodulation::DecodeRemoteVisualFrame(view, state.remoteVisual, state.hard, state.soft, policy);
    if (result.modulation.IsAccepted())
    {
        result.evaluation = EvaluateCodewords(result.modulation.bootstrap.canonical44,
            std::span(state.hard).first(result.modulation.dataBytes),
            std::span(state.soft).first(static_cast<std::size_t>(result.modulation.dataBytes) * 8));
        state.remoteVisualHistogramValid = result.evaluation.evaluated;
    }
    return result;
}

FrameEvaluation ReferenceChannel::EvaluateCodewords(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> hardData, const std::span<const float> softMetrics, const EvaluationMode mode) noexcept
{
    FrameEvaluation result;
    if (!implementation_)
    {
        return result;
    }
    auto& state = *implementation_;
    state.histogramValid = false;
    state.shapeHistogramValid = false;
    state.remoteVisualHistogramValid = false;
    state.acceptedCount = 0;
    state.acceptedRemoteControlCount = 0;
    pbprotocol::BootstrapRecord record;
    const auto* const profile = ParseBinding(bootstrapRecord, record);
    if ((mode != EvaluationMode::DiagnosticTruth && mode != EvaluationMode::Transport) || profile == nullptr ||
        hardData.size() != profile->dataBytes || softMetrics.size() != profile->dataBytes * 8ULL ||
        hardData.data() == nullptr || softMetrics.data() == nullptr)
    {
        return result;
    }
    // Frozen receiver-local baseline: normalized min-sum, 48 passes maximum.
    // The Robust wire matrix/profile remains unchanged.
    pbinnerfec::InnerFecDecodeOptions options;
    options.offset = 0;
    options.scaleNum = 3;
    options.scaleDen = 4;
    state.crcValid.fill(false);
    result.codewords = profile->codewords;
    for (std::uint32_t slot = 0; slot < profile->codewords; slot++)
    {
        const auto recovered = std::span(state.recovered).subspan(slot * kCodewordBytes, kCodewordBytes);
        const auto codewordMetrics = softMetrics.subspan(slot * kCodewordBits, kCodewordBits);
        // A zero syndrome already satisfies the full QC-LDPC parity system.
        // Derive this candidate from the exact fixed-adapter rounding boundary,
        // never hardData or known truth: zero/erased soft input must not borrow
        // correct hard bits. Full LLR adaptation remains mandatory before any
        // iterative decode. CRC, identity and truth scoring remain mandatory.
        constexpr float negativeDecisionThreshold = static_cast<float>(-0.5 / kSoftMetricScale);
        for (std::size_t byte = 0; byte < recovered.size(); byte++)
        {
            unsigned value = 0;
            for (std::size_t bit = 0; bit < 8; bit++)
            {
                const float metric = codewordMetrics[byte * 8 + bit];
                if (!std::isfinite(metric))
                {
                    state.acceptedCount = 0;
                    state.acceptedRemoteControlCount = 0;
                    return {};
                }
                value |= static_cast<unsigned>(metric <= negativeDecisionThreshold) << bit;
            }
            recovered[byte] = static_cast<std::byte>(value);
        }
        const auto initialSyndrome = pbinnerfec::ComputeQcLdpcSyndrome(pbinnerfec::kInnerFecProfileIdRobust, recovered);
        if (!initialSyndrome)
        {
            result.fecFailures++;
            continue;
        }
        if (!initialSyndrome.Value())
        {
            AdaptFiniteSoftMetrics(codewordMetrics, state.llr);
            const auto decoded = state.decoder.Decode(state.llr, options, recovered);
            const auto iterations = decoded ? decoded.Value().iterationsUsed :
                (decoded.Error().code == pbinnerfec::InnerFecErrorCode::SyndromeFailure ? options.maxIterations : 0);
            result.iterationsTotal += iterations; // <= 42 * 48
            result.iterationsMaximum = std::max(result.iterationsMaximum, iterations);
            if (!decoded)
            {
                result.fecFailures++;
                continue;
            }
        }
        const auto block = pbprotocol::ExtractTransportBlockFromInfoBlock(recovered.first(kInfoBytes));
        if (!block)
        {
            AcceptedRemoteControlBlock control;
            const RemoteControlExtractResult controlResult = mode == EvaluationMode::Transport &&
                profile->profileId == remoteVisualBinding.profileId ?
                ExtractRemoteControl(recovered.first(kInfoBytes), record.sessionTag, control) :
                RemoteControlExtractResult::Invalid;
            if (controlResult == RemoteControlExtractResult::Accepted)
            {
                state.crcValid[slot] = true;
                state.acceptedRemoteControl[state.acceptedRemoteControlCount++] = control;
                result.acceptedRemoteControlBlocks++;
                continue;
            }
            if (controlResult == RemoteControlExtractResult::IdentityMismatch)
            {
                result.identityFailures++;
                continue;
            }
            result.crcFailures++;
            continue;
        }
        const auto transport = pbprotocol::ParseTransportBlock(block.Value());
        if (!transport)
        {
            result.crcFailures++;
            continue;
        }
        state.crcValid[slot] = true;
        const auto& header = transport.Value().header;
        const bool identityValid = mode == EvaluationMode::DiagnosticTruth ?
            header.sessionTag == record.sessionTag && header.segmentOrdinal == record.frameSequence &&
                header.outerBlockId == slot && header.payloadBytes == kPayloadBytes :
            header.sessionTag == record.sessionTag;
        if (!identityValid)
        {
            result.identityFailures++;
        }
        else
        {
            auto& accepted = state.accepted[state.acceptedCount++];
            accepted.slot = slot;
            accepted.byteCount = static_cast<std::uint32_t>(block.Value().size());
            accepted.bytes.fill(std::byte{0});
            std::copy(block.Value().begin(), block.Value().end(), accepted.bytes.begin());
            result.acceptedTransportBlocks++;
        }
    }

    const std::size_t codedBytes = profile->codewords * kCodewordBytes;
    if (!std::ranges::all_of(softMetrics.subspan(codedBytes * 8), [](const float metric) { return std::isfinite(metric); }))
    {
        state.acceptedCount = 0;
        state.acceptedRemoteControlCount = 0;
        return {};
    }
    if (mode == EvaluationMode::DiagnosticTruth)
    {
        // Only diagnostic scoring below this line may consult known data. No
        // recovered buffer or decoder input is modified by this comparison.
        const auto expected = std::span(state.expected).first(profile->dataBytes);
        if (!GenerateInto(bootstrapRecord, record, *profile, expected))
        {
            return {};
        }
        result.comparedCodedBits = codedBytes * 8;
        for (std::size_t index = 0; index < codedBytes; index++)
        {
            result.erroneousCodedBits += std::popcount(std::to_integer<unsigned>(hardData[index] ^ expected[index]));
        }
        for (std::uint32_t slot = 0; slot < profile->codewords; slot++)
        {
            const auto offset = slot * kCodewordBytes;
            // Count ANY CRC-valid non-truth codeword, including a different
            // valid identity, as a dangerous false-accept candidate.
            if (state.crcValid[slot] &&
                !std::equal(state.recovered.begin() + offset, state.recovered.begin() + offset + kCodewordBytes, expected.begin() + offset))
            {
                result.falseAcceptedCodewords++;
            }
        }
    }
    result.paddingValid = std::ranges::all_of(hardData.subspan(codedBytes), [](const std::byte value)
    {
        return value == std::byte{0};
    });
    result.evaluated = true;
    return result;
}

std::span<const std::uint64_t> ReferenceChannel::GetMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? implementation_->modulation.GetMarginHistogram() : std::span<const std::uint64_t>{};
}

std::span<const std::uint64_t> ReferenceChannel::GetShapeMarginHistogram() const noexcept
{
    return implementation_ && implementation_->shapeHistogramValid ? implementation_->shapeChroma.GetShapeMarginHistogram() :
        std::span<const std::uint64_t>{};
}

std::span<const std::uint64_t> ReferenceChannel::GetChromaMarginHistogram() const noexcept
{
    return implementation_ && implementation_->shapeHistogramValid ? implementation_->shapeChroma.GetChromaMarginHistogram() :
        std::span<const std::uint64_t>{};
}

std::span<const std::uint64_t> ReferenceChannel::GetRemoteVisualMarginHistogram() const noexcept
{
    return implementation_ && implementation_->remoteVisualHistogramValid ?
        implementation_->remoteVisual.GetMarginHistogram() : std::span<const std::uint64_t>{};
}

std::span<const AcceptedTransportBlock> ReferenceChannel::GetAcceptedTransportBlocks() const noexcept
{
    return implementation_ ? std::span<const AcceptedTransportBlock>(implementation_->accepted).first(implementation_->acceptedCount) :
        std::span<const AcceptedTransportBlock>{};
}

std::span<const AcceptedRemoteControlBlock> ReferenceChannel::GetAcceptedRemoteControlBlocks() const noexcept
{
    return implementation_ ? std::span<const AcceptedRemoteControlBlock>(implementation_->acceptedRemoteControl)
        .first(implementation_->acceptedRemoteControlCount) : std::span<const AcceptedRemoteControlBlock>{};
}

bool ReferenceStatistics::Add(const FrameEvaluation& evaluation, const std::uint64_t sequence,
    const std::span<const std::uint64_t> histogram, const double minimumMargin) noexcept
{
    const std::uint64_t terminalFailures = static_cast<std::uint64_t>(evaluation.fecFailures) + evaluation.crcFailures + evaluation.identityFailures;
    if (!evaluation.evaluated || (evaluation.codewords != 10 && evaluation.codewords != 32 && evaluation.codewords != 42) ||
        evaluation.comparedCodedBits != evaluation.codewords * kCodewordBits || evaluation.erroneousCodedBits > evaluation.comparedCodedBits ||
        terminalFailures > evaluation.codewords || static_cast<std::uint64_t>(evaluation.acceptedTransportBlocks) + terminalFailures != evaluation.codewords ||
        evaluation.falseAcceptedCodewords > evaluation.codewords - evaluation.fecFailures - evaluation.crcFailures ||
        evaluation.iterationsTotal > evaluation.codewords * 48 || evaluation.iterationsMaximum > 48 ||
        histogram.size() != histogram_.size() || histogram.data() == nullptr || !std::isfinite(minimumMargin) || minimumMargin < 0 || minimumMargin > 1)
    {
        return false;
    }
    const auto margin = pbmodulation::SummarizeDesktopLevelsMargin(histogram, minimumMargin);
    const std::uint64_t expectedTiles = evaluation.codewords == 42 ? 346752 : 86688;
    if (margin.samples != expectedTiles)
    {
        return false;
    }
    StatisticsSummary next = summary_;
    const auto Add = [](std::uint64_t& counter, const std::uint64_t amount)
    {
        const auto sum = pbprotocol::CheckedAddUnsigned(counter, amount);
        if (!sum)
        {
            return false;
        }
        counter = sum.Value();
        return true;
    };
    if (!Add(next.frames, 1) || !Add(next.verifiedFrames, evaluation.IsVerified() ? 1 : 0) ||
        !Add(next.comparedCodedBits, evaluation.comparedCodedBits) || !Add(next.erroneousCodedBits, evaluation.erroneousCodedBits) ||
        !Add(next.preFecFailedFrames, evaluation.erroneousCodedBits != 0 ? 1 : 0) || !Add(next.postFecFailedFrames, evaluation.IsVerified() ? 0 : 1) ||
        !Add(next.falseAcceptedCodewords, evaluation.falseAcceptedCodewords) || !Add(next.codewords, evaluation.codewords) ||
        !Add(next.fecFailures, evaluation.fecFailures) || !Add(next.crcFailures, evaluation.crcFailures) || !Add(next.identityFailures, evaluation.identityFailures) ||
        !Add(next.paddingFailedFrames, evaluation.paddingValid ? 0 : 1) || !Add(next.iterationsTotal, evaluation.iterationsTotal) ||
        !Add(next.margin.samples, margin.samples))
    {
        return false;
    }
    for (std::size_t index = 0; index < histogram_.size(); index++)
    {
        if (histogram[index] > std::numeric_limits<std::uint64_t>::max() - histogram_[index])
        {
            return false;
        }
    }
    for (std::size_t index = 0; index < histogram_.size(); index++)
    {
        histogram_[index] += histogram[index];
    }
    next.iterationsMaximum = std::max(next.iterationsMaximum, evaluation.iterationsMaximum);
    const auto phase = static_cast<std::uint16_t>(1u << (sequence % 16));
    next.observedPhases |= phase;
    if (evaluation.IsVerified())
    {
        next.verifiedPhases |= phase;
    }
    next.margin.minimum = summary_.margin.samples == 0 ? minimumMargin : std::min(summary_.margin.minimum, minimumMargin);
    summary_ = next;
    return true;
}

StatisticsSummary ReferenceStatistics::GetSummary() const noexcept
{
    StatisticsSummary result = summary_;
    result.margin = pbmodulation::SummarizeDesktopLevelsMargin(histogram_, summary_.margin.minimum);
    return result;
}

void ReferenceStatistics::Reset() noexcept
{
    summary_ = {};
    histogram_.fill(0);
}

void WriteStatisticsJson(std::ostream& output, const StatisticsSummary& statistics)
{
    output << "{\"frames\":" << statistics.frames << ",\"verifiedFrames\":" << statistics.verifiedFrames
        << ",\"comparedCodedBits\":" << statistics.comparedCodedBits << ",\"erroneousCodedBits\":" << statistics.erroneousCodedBits
        << ",\"PreFecBER\":";
    WriteRatio(output, statistics.erroneousCodedBits, statistics.comparedCodedBits);
    output << ",\"preFecFailedFrames\":" << statistics.preFecFailedFrames << ",\"PreFecFER\":";
    WriteRatio(output, statistics.preFecFailedFrames, statistics.frames);
    output << ",\"postFecFailedFrames\":" << statistics.postFecFailedFrames << ",\"PostFecFER\":";
    WriteRatio(output, statistics.postFecFailedFrames, statistics.frames);
    output << ",\"falseAcceptedCodewords\":" << statistics.falseAcceptedCodewords << ",\"codewords\":" << statistics.codewords
        << ",\"fecFailures\":" << statistics.fecFailures << ",\"crcFailures\":" << statistics.crcFailures
        << ",\"identityFailures\":" << statistics.identityFailures << ",\"paddingFailedFrames\":" << statistics.paddingFailedFrames
        << ",\"iterationsTotal\":" << statistics.iterationsTotal << ",\"iterationsMaximum\":" << statistics.iterationsMaximum
        << ",\"observedPhases\":" << statistics.observedPhases << ",\"verifiedPhases\":" << statistics.verifiedPhases
        << ",\"softMetric\":\"uncalibrated-max-log-distance\",\"int16Scale\":4096,\"int16Clip\":32767,\"margin\":";
    WriteMargin(output, statistics.margin);
    output << '}';
}

void WriteEvaluationJson(std::ostream& output, const FrameEvaluation& evaluation)
{
    output << "{\"evaluated\":" << (evaluation.evaluated ? "true" : "false") << ",\"verified\":" << (evaluation.IsVerified() ? "true" : "false")
        << ",\"paddingValid\":" << (evaluation.paddingValid ? "true" : "false") << ",\"codewords\":" << evaluation.codewords
        << ",\"comparedCodedBits\":" << evaluation.comparedCodedBits << ",\"erroneousCodedBits\":" << evaluation.erroneousCodedBits
        << ",\"fecFailures\":" << evaluation.fecFailures << ",\"crcFailures\":" << evaluation.crcFailures
        << ",\"identityFailures\":" << evaluation.identityFailures << ",\"falseAcceptedCodewords\":" << evaluation.falseAcceptedCodewords
        << ",\"acceptedTransportBlocks\":" << evaluation.acceptedTransportBlocks
        << ",\"iterationsTotal\":" << evaluation.iterationsTotal << ",\"iterationsMaximum\":" << evaluation.iterationsMaximum << '}';
}

void WriteModulationJson(std::ostream& output, const pbmodulation::DesktopLevelsObservation& observation)
{
    output << "{\"erasure\":\"" << pbmodulation::GetDesktopLevelsErasureName(observation.erasure)
        << "\",\"bootstrapErasure\":\"" << pbmodulation::GetLocalDesktopErasureName(observation.bootstrap.erasure)
        << "\",\"profileId\":\"" << observation.profileId << "\",\"identity\":";
    const auto record = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (observation.bootstrap.IsAccepted() && record)
    {
        output << "{\"SessionTag\":\"" << record.Value().sessionTag.value << "\",\"FrameSequence\":\"" << record.Value().frameSequence
               << "\",\"interleavePhase\":" << record.Value().frameSequence % 16 << '}';
    }
    else
    {
        output << "null";
    }
    output << ",\"dataBytes\":" << observation.dataBytes << ",\"dataWorkUnits\":" << observation.dataWorkUnits << ",\"scaleX\":";
    WriteNumber(output, observation.bootstrap.geometry.scaleX);
    output << ",\"scaleY\":";
    WriteNumber(output, observation.bootstrap.geometry.scaleY);
    output << ",\"originX\":";
    WriteNumber(output, observation.bootstrap.geometry.originX);
    output << ",\"originY\":";
    WriteNumber(output, observation.bootstrap.geometry.originY);
    output << ",\"markerResidualPixels\":";
    WriteNumber(output, observation.bootstrap.geometry.markerResidualPixels);
    output << ",\"phaseResidual\":";
    WriteNumber(output, observation.calibration.phaseResidual);
    output << ",\"calibration\":{\"centroids\":[";
    for (std::size_t index = 0; index < 4; index++)
    {
        if (index != 0)
        {
            output << ',';
        }
        WriteNumber(output, observation.calibration.centroids[index]);
    }
    output << "],\"variances\":[";
    for (std::size_t index = 0; index < 4; index++)
    {
        if (index != 0)
        {
            output << ',';
        }
        WriteNumber(output, observation.calibration.variances[index]);
    }
    output << "],\"minimumGap\":";
    WriteNumber(output, observation.calibration.minimumGap);
    output << ",\"spatialDeviation\":";
    WriteNumber(output, observation.calibration.spatialDeviation);
    output << ",\"ladderCentroids\":";
    WriteMatrix(output, observation.calibration.ladderCentroids);
    output << ",\"ladderVariances\":";
    WriteMatrix(output, observation.calibration.ladderVariances);
    output << ",\"phaseResiduals\":";
    WriteMatrix(output, observation.calibration.phaseResiduals);
    output << "},\"unreliableTiles\":" << observation.unreliableTiles << ",\"margin\":";
    WriteMargin(output, observation.margin);
    output << '}';
}
} // namespace pbdesktoplevels
