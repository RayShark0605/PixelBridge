#include "pbmodulation/experimental_pam4.h"

#include "experimental_pam4_mapping.h"
#include "local_desktop_internal.h"
#include "luma_reader.h"
#include "unified_reserved_raster.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pbmodulation
{
namespace
{
constexpr std::array<double, 4> kCoarseLevels{8, 64, 160, 232};
constexpr std::array<double, 4> kFineLevels{56, 112, 168, 248};
constexpr std::array<double, 6> kAnchorLevels{0, 8, 64, 160, 232, 255};
constexpr std::array<std::uint32_t, 4> kCalibrationX{736, 1696, 96, 1056};
constexpr std::array<std::uint32_t, 4> kCalibrationY{16, 16, 1000, 1000};
constexpr std::uint64_t kSampleWorkLimit = 1000000;
static_assert(detail::kExperimentalPam4CellSites.size() == kExperimentalPam4CellCount);
static_assert(detail::kExperimentalPam4BitPositions.size() == kExperimentalPam4MetricCount);

bool IsPam4Pair(const std::uint64_t profile, const std::uint8_t layout) noexcept
{
    return profile == pbprotocol::kPam4ExperimentalProfile.visualProfileId && layout == pbprotocol::kPam4ExperimentalProfile.visualLayoutVersion;
}

bool Overlap(const std::span<const std::byte> left, const std::span<const std::byte> right) noexcept
{
    const auto leftBegin = reinterpret_cast<std::uintptr_t>(left.data());
    const auto rightBegin = reinterpret_cast<std::uintptr_t>(right.data());
    const auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    return left.size() > maximum - leftBegin || right.size() > maximum - rightBegin ||
        (leftBegin < rightBegin + right.size() && rightBegin < leftBegin + left.size());
}

ModulationStatus Invalid(const std::size_t offset = 0) noexcept
{
    return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, offset);
}

bool ControlPriorityMatches(const pbprotocol::ControlRecordType type, const UnifiedControlPriority priority) noexcept
{
    switch (type)
    {
    case pbprotocol::ControlRecordType::SessionDescriptor: return priority == UnifiedControlPriority::SessionDescriptor;
    case pbprotocol::ControlRecordType::SegmentDescriptor: return priority == UnifiedControlPriority::CurrentSegmentDescriptor;
    case pbprotocol::ControlRecordType::FinalManifest: return priority == UnifiedControlPriority::FinalManifest;
    }
    return false;
}

template<std::size_t Size>
double Median(std::array<double, Size> values) noexcept
{
    static_assert(Size % 2 == 0);
    std::sort(values.begin(), values.end());
    return (values[Size / 2 - 1] + values[Size / 2]) / 2;
}

// Preserve the measured reference's operation order and half-pixel transform.
// LumaReader validates/charges every pixel; there is no full-frame conversion.
bool Sample(detail::LumaReader& reader, const LumaView& view, const LocalDesktopGeometry& geometry,
    const double logicalX, const double logicalY, double& output) noexcept
{
    const double x = logicalX * geometry.scaleX + geometry.originX - 0.5;
    const double y = logicalY * geometry.scaleY + geometry.originY - 0.5;
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= view.width - 1.0 || y >= view.height - 1.0)
    {
        return false;
    }
    const auto left = static_cast<std::uint32_t>(std::floor(x));
    const auto top = static_cast<std::uint32_t>(std::floor(y));
    const double fractionX = x - left;
    const double fractionY = y - top;
    std::array<double, 4> values{};
    if (!reader.Pixel(left, top, values[0]) || !reader.Pixel(left + 1, top, values[1]) ||
        !reader.Pixel(left, top + 1, values[2]) || !reader.Pixel(left + 1, top + 1, values[3]))
    {
        return false;
    }
    const double upper = values[0] * (1 - fractionX) + values[1] * fractionX;
    const double lower = values[2] * (1 - fractionX) + values[3] * fractionX;
    output = upper * (1 - fractionY) + lower * fractionY;
    return std::isfinite(output);
}

double Interpolate(const double value, const std::array<double, 6>& anchors) noexcept
{
    if (value <= kAnchorLevels.front())
    {
        return anchors.front();
    }
    if (value >= kAnchorLevels.back())
    {
        return anchors.back();
    }
    for (std::size_t index = 1; index < anchors.size(); index++)
    {
        if (value <= kAnchorLevels[index])
        {
            const double slope = (anchors[index] - anchors[index - 1]) / (kAnchorLevels[index] - kAnchorLevels[index - 1]);
            return slope * (value - kAnchorLevels[index - 1]) + anchors[index - 1];
        }
    }
    return anchors.back();
}

bool Calibrate(detail::LumaReader& reader, const LumaView& view, const LocalDesktopGeometry& geometry,
    ExperimentalPam4Calibration& output) noexcept
{
    ExperimentalPam4Calibration calibration;
    for (std::size_t region = 0; region < 4; region++)
    {
        for (std::size_t level = 0; level < 4; level++)
        {
            std::array<double, 12> coarseSamples{};
            std::array<double, 12> fineSamples{};
            for (std::uint32_t row = 0; row < 3; row++)
            {
                for (std::uint32_t column = 0; column < 4; column++)
                {
                    const double logicalX = static_cast<double>(kCalibrationX[region] + level * 32 + 6 + column * 6);
                    if (!Sample(reader, view, geometry, logicalX, kCalibrationY[region] + 6 + row * 6, coarseSamples[row * 4 + column]) ||
                        !Sample(reader, view, geometry, logicalX, kCalibrationY[region] + 40 + row * 8, fineSamples[row * 4 + column]))
                    {
                        return false;
                    }
                }
            }
            calibration.coarse[region][level] = Median(coarseSamples);
            calibration.heldOutFine[region][level] = Median(fineSamples);
            if (level > 0 && calibration.coarse[region][level] - calibration.coarse[region][level - 1] < 16)
            {
                return false;
            }
        }
        std::array<double, 8> neutralSamples{};
        for (std::uint32_t index = 0; index < neutralSamples.size(); index++)
        {
            if (!Sample(reader, view, geometry, kCalibrationX[region] + 8 + index * 16, kCalibrationY[region] + 28, neutralSamples[index]))
            {
                return false;
            }
        }
        calibration.heldOutNeutral[region] = Median(neutralSamples);
    }
    std::array<double, 4> medians{};
    for (std::size_t level = 0; level < 4; level++)
    {
        medians[level] = Median(std::array<double, 4>{calibration.coarse[0][level], calibration.coarse[1][level], calibration.coarse[2][level], calibration.coarse[3][level]});
        for (std::size_t region = 0; region < 4; region++)
        {
            if (std::abs(calibration.coarse[region][level] - medians[level]) > 8)
            {
                return false;
            }
        }
    }
    const double dark = medians[0] - kCoarseLevels[0] * (medians[1] - medians[0]) / (kCoarseLevels[1] - kCoarseLevels[0]);
    const double light = medians[3] + (255 - kCoarseLevels[3]) * (medians[3] - medians[2]) / (kCoarseLevels[3] - kCoarseLevels[2]);
    const std::array<double, 6> anchors{std::clamp(dark, 0.0, 255.0), medians[0], medians[1], medians[2], medians[3], std::clamp(light, 0.0, 255.0)};
    if (anchors.back() - anchors.front() < 96)
    {
        return false;
    }
    for (std::size_t index = 1; index < anchors.size(); index++)
    {
        if (anchors[index] < anchors[index - 1])
        {
            return false;
        }
    }
    for (std::size_t level = 0; level < 4; level++)
    {
        calibration.dataCentroids[level] = Interpolate(kExperimentalPam4Levels[level], anchors);
        if (level > 0 && calibration.dataCentroids[level] - calibration.dataCentroids[level - 1] < 16)
        {
            return false;
        }
    }
    for (std::size_t region = 0; region < 4; region++)
    {
        for (std::size_t level = 0; level < 4; level++)
        {
            calibration.maximumHeldOutResidual = (std::max)(calibration.maximumHeldOutResidual,
                std::abs(calibration.heldOutFine[region][level] - Interpolate(kFineLevels[level], anchors)));
        }
        calibration.maximumHeldOutResidual = (std::max)(calibration.maximumHeldOutResidual,
            std::abs(calibration.heldOutNeutral[region] - Interpolate(128, anchors)));
    }
    if (calibration.maximumHeldOutResidual > 8)
    {
        return false;
    }
    calibration.valid = true;
    output = calibration;
    return true;
}

bool Freshness(const LumaView& view, const LocalDesktopObservation& bootstrap,
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount>& observations) noexcept
{
    const double contrast = bootstrap.whiteLevel - bootstrap.blackLevel;
    if (!(contrast > 0))
    {
        return false;
    }
    bool allCurrent = true;
    for (std::uint32_t patch = 0; patch < observations.size(); patch++)
    {
        std::array<std::uint8_t, kLocalDesktopTimingBits> expected{};
        if (!BuildUnifiedFreshnessBits(bootstrap.canonical44, patch, expected))
        {
            return false;
        }
        const auto region = kLocalDesktopTimingRegions[patch];
        double residual = 0;
        std::uint16_t errors = 0;
        for (std::uint32_t bit = 0; bit < expected.size(); bit++)
        {
            double sum = 0;
            for (std::uint32_t row = 2; row < 6; row++)
            {
                for (std::uint32_t column = 2; column < 6; column++)
                {
                    double x = bootstrap.geometry.originX + bootstrap.geometry.scaleX * (region.x + bit % 16 * 8 + column + 0.5) - 0.5;
                    double y = bootstrap.geometry.originY + bootstrap.geometry.scaleY * (region.y + bit / 16 * 8 + row + 0.5) - 0.5;
                    if (bootstrap.geometry.scaleX < 1)
                    {
                        x = std::floor(x + 0.5);
                    }
                    if (bootstrap.geometry.scaleY < 1)
                    {
                        y = std::floor(y + 0.5);
                    }
                    double value = 0;
                    if (SampleLuma(view, x, y, value) != LocalDesktopErasureReason::None)
                    {
                        return false;
                    }
                    sum += value;
                }
            }
            const double normalized = (sum / 16 - bootstrap.blackLevel) / contrast;
            residual += std::abs(normalized - expected[bit]);
            errors += static_cast<std::uint16_t>((normalized >= 0.5) != (expected[bit] != 0));
        }
        observations[patch] = {static_cast<double>(errors) / expected.size() <= 0.02 && residual / expected.size() <= 0.075,
            errors, residual / expected.size()};
        allCurrent = allCurrent && observations[patch].current;
    }
    return allCurrent;
}

std::int16_t Quantize(const double value) noexcept
{
    const double bounded = std::clamp(value, -32767.0, 32767.0);
    const double lower = std::floor(bounded);
    const double fraction = bounded - lower;
    const auto integer = static_cast<std::int32_t>(lower);
    const bool roundUp = fraction > 0.5 || (fraction == 0.5 && integer % 2 != 0);
    return static_cast<std::int16_t>(integer + static_cast<std::int32_t>(roundUp));
}

UnifiedSlotRejection ProtocolRejection(const pbprotocol::ProtocolErrorCode code, const bool control) noexcept
{
    if (code == pbprotocol::ProtocolErrorCode::NonCanonicalPadding)
    {
        return UnifiedSlotRejection::NonCanonicalPadding;
    }
    if (code == pbprotocol::ProtocolErrorCode::CrcMismatch)
    {
        return control ? UnifiedSlotRejection::ControlCrcFailure : UnifiedSlotRejection::TransportCrcFailure;
    }
    return UnifiedSlotRejection::InvalidInformation;
}
}

std::uint32_t GetExperimentalPam4CellSite(const std::uint32_t cellOrdinal) noexcept
{
    return cellOrdinal < detail::kExperimentalPam4CellSites.size() ? detail::kExperimentalPam4CellSites[cellOrdinal] : (std::numeric_limits<std::uint32_t>::max)();
}

std::uint32_t GetExperimentalPam4BitPosition(const std::uint32_t logicalBit) noexcept
{
    return logicalBit < detail::kExperimentalPam4BitPositions.size() ? detail::kExperimentalPam4BitPositions[logicalBit] : (std::numeric_limits<std::uint32_t>::max)();
}

bool ValidateExperimentalPam4SlotPlan(const std::span<const UnifiedSlotAssignment> assignments) noexcept
{
    if (assignments.size() != kExperimentalPam4CodewordCount || assignments.data() == nullptr)
    {
        return false;
    }
    std::array<bool, kExperimentalPam4CodewordCount> seen{};
    for (const auto& assignment : assignments)
    {
        const auto slot = assignment.codewordSlot;
        if (slot >= seen.size() || seen[slot] ||
            (slot == 0 && (assignment.kind != UnifiedSlotKind::Control || !IsUnifiedControlPriority(assignment.controlPriority))) ||
            (slot != 0 && (assignment.kind != UnifiedSlotKind::Transport || assignment.controlPriority != UnifiedControlPriority::NotApplicable)))
        {
            return false;
        }
        seen[slot] = true;
    }
    return true;
}

ModulationStatus PackExperimentalPam4Frame(const UnifiedVisualFrameInput& input, const std::span<std::byte> output) noexcept
{
    if (input.bootstrapRecord.data() == nullptr || input.bootstrapRecord.size() != pbprotocol::kBootstrapRecordBytes ||
        input.slots.data() == nullptr || input.slots.size() != kExperimentalPam4CodewordCount || output.data() == nullptr || output.size() != kExperimentalPam4CodedFrameBytes)
    {
        return Invalid();
    }
    const auto bootstrap = pbprotocol::ParseBootstrapRecord(input.bootstrapRecord);
    if (!bootstrap || !IsPam4Pair(bootstrap.Value().visualProfileId, bootstrap.Value().visualLayoutVersion))
    {
        return Invalid(8);
    }
    std::array<UnifiedSlotAssignment, kExperimentalPam4CodewordCount> assignments{};
    for (std::size_t index = 0; index < assignments.size(); index++)
    {
        assignments[index] = input.slots[index].assignment;
    }
    if (!ValidateExperimentalPam4SlotPlan(assignments))
    {
        return Invalid();
    }
    std::array<std::byte, kExperimentalPam4CodedFrameBytes> staged{};
    std::array<std::byte, kExperimentalPam4DataInformationBytes> information{};
    for (const auto& slotInput : input.slots)
    {
        const auto slot = slotInput.assignment.codewordSlot;
        const bool control = slot == 0;
        const auto informationBytes = control ? kExperimentalPam4ControlInformationBytes : kExperimentalPam4DataInformationBytes;
        const auto profile = control ? pbinnerfec::kInnerFecProfileIdRobust : pbinnerfec::kInnerFecProfileIdFast;
        std::fill(information.begin(), information.end(), std::byte{0});
        if (!slotInput.active)
        {
            if (control || !slotInput.block.empty())
            {
                return Invalid(slot);
            }
        }
        else
        {
            if (slotInput.block.data() == nullptr || slotInput.block.empty() || slotInput.block.size() > informationBytes)
            {
                return Invalid(slot);
            }
            if (control)
            {
                const auto record = pbprotocol::ParseControlRecord(slotInput.block);
                if (!record || record.Value().sessionTag != bootstrap.Value().sessionTag ||
                    !ControlPriorityMatches(record.Value().recordType, slotInput.assignment.controlPriority))
                {
                    return Invalid(slot);
                }
            }
            else
            {
                const auto record = pbprotocol::ParseTransportBlock(slotInput.block);
                if (!record || record.Value().header.sessionTag != bootstrap.Value().sessionTag)
                {
                    return Invalid(slot);
                }
            }
            const auto status = control ? pbprotocol::FrameControlRecordIntoInfoBlock(slotInput.block, informationBytes, std::span(information).first(informationBytes)) :
                pbprotocol::FrameTransportBlockIntoInfoBlock(slotInput.block, informationBytes, std::span(information).first(informationBytes));
            if (!status)
            {
                return Invalid(slot);
            }
        }
        const auto encoded = pbinnerfec::EncodeQcLdpcCodeword(profile, std::span(information).first(informationBytes),
            std::span(staged).subspan(static_cast<std::size_t>(slot) * kExperimentalPam4CodewordBytes, kExperimentalPam4CodewordBytes));
        if (!encoded)
        {
            return ModulationStatus::Failure(ModulationErrorCode::InternalInvariantViolation, slot);
        }
    }
    std::copy(staged.begin(), staged.end(), output.begin());
    return ModulationStatus::Success();
}

ModulationStatus EncodeExperimentalPam4Frame(const UnifiedVisualFrameInput& input, const std::span<std::byte> outBgra) noexcept
{
    std::array<std::byte, kExperimentalPam4CodedFrameBytes> coded{};
    const auto packed = PackExperimentalPam4Frame(input, coded);
    return packed ? EncodeExperimentalPam4Frame(input.bootstrapRecord, coded, outBgra) : packed;
}

ModulationStatus EncodeExperimentalPam4Frame(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> codedFrame, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr || bootstrapRecord.size() != pbprotocol::kBootstrapRecordBytes ||
        codedFrame.data() == nullptr || codedFrame.size() != kExperimentalPam4CodedFrameBytes ||
        outBgra.data() == nullptr || outBgra.size() != kUnifiedFrameBgraBytes || Overlap(bootstrapRecord, outBgra) || Overlap(codedFrame, outBgra))
    {
        return Invalid();
    }
    const auto bootstrap = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!bootstrap || !IsPam4Pair(bootstrap.Value().visualProfileId, bootstrap.Value().visualLayoutVersion))
    {
        return Invalid(8);
    }
    std::array<std::uint8_t, kExperimentalPam4CellCount> labels{};
    for (std::uint32_t bit = 0; bit < kExperimentalPam4MetricCount; bit++)
    {
        const auto position = detail::kExperimentalPam4BitPositions[bit];
        const auto value = (std::to_integer<std::uint8_t>(codedFrame[bit / 8]) >> (bit % 8)) & 1U;
        labels[position / 2] |= static_cast<std::uint8_t>(value << (position % 2));
    }
    const auto scaffold = detail::EncodeLocalDesktopScaffold(bootstrapRecord, outBgra, detail::LocalDesktopBinding::ExperimentalPam4);
    if (!scaffold)
    {
        return scaffold;
    }
    // Only fixed-bounds stores remain. The scaffold has already painted every
    // pixel neutral; partial Data cells and all guards remain untouched.
    detail::RenderUnifiedReservedPilots(outBgra, true, bootstrap.Value().frameSequence);
    for (std::uint32_t cell = 0; cell < kExperimentalPam4CellCount; cell++)
    {
        const auto site = detail::kExperimentalPam4CellSites[cell];
        detail::FillLocalDesktopBlock(outBgra, {site % 480 * 4, site / 480 * 4, 4, 4}, kExperimentalPam4Levels[labels[cell]]);
    }
    return ModulationStatus::Success();
}

struct ExperimentalPam4CpuDecoder::Implementation
{
    std::array<std::int16_t, kExperimentalPam4CellCount * 2> physicalMetrics{};
    std::array<std::int16_t, kExperimentalPam4MetricCount> logicalMetrics{};
    std::array<std::byte, kExperimentalPam4CodewordBytes> decoded{};
    std::array<UnifiedAcceptedBlock, kExperimentalPam4CodewordCount> accepted{};
    std::uint32_t acceptedCount = 0;
    bool metricsValid = false;
    LocalDesktopGeometry geometryCache;
    ExperimentalPam4Calibration calibration;
    pbinnerfec::QcLdpcDecoder controlDecoder;
    pbinnerfec::QcLdpcDecoder dataDecoder;
};

ExperimentalPam4CpuDecoder::ExperimentalPam4CpuDecoder() noexcept = default;
ExperimentalPam4CpuDecoder::ExperimentalPam4CpuDecoder(ExperimentalPam4CpuDecoder&&) noexcept = default;
ExperimentalPam4CpuDecoder& ExperimentalPam4CpuDecoder::operator=(ExperimentalPam4CpuDecoder&&) noexcept = default;
ExperimentalPam4CpuDecoder::~ExperimentalPam4CpuDecoder() = default;

std::uint64_t ExperimentalPam4CpuDecoder::RequiredBytes() noexcept
{
    return sizeof(Implementation) + 2U * 1024U * 1024U;
}

ModulationResult<ExperimentalPam4CpuDecoder> ExperimentalPam4CpuDecoder::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<ExperimentalPam4CpuDecoder>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    try
    {
        ExperimentalPam4CpuDecoder decoder;
        decoder.implementation_ = std::make_unique<Implementation>();
        auto control = pbinnerfec::QcLdpcDecoder::Create(pbinnerfec::kInnerFecProfileIdRobust);
        auto data = pbinnerfec::QcLdpcDecoder::Create(pbinnerfec::kInnerFecProfileIdFast);
        if (!control || !data)
        {
            const auto error = !control ? control.Error().code : data.Error().code;
            return ModulationResult<ExperimentalPam4CpuDecoder>::Failure(error == pbinnerfec::InnerFecErrorCode::OutOfMemory ?
                ModulationErrorCode::MemoryAllocationFailure : ModulationErrorCode::InternalInvariantViolation, 0);
        }
        decoder.implementation_->controlDecoder = std::move(control.Value());
        decoder.implementation_->dataDecoder = std::move(data.Value());
        return ModulationResult<ExperimentalPam4CpuDecoder>::Success(std::move(decoder));
    }
    catch (...)
    {
        return ModulationResult<ExperimentalPam4CpuDecoder>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

void ExperimentalPam4CpuDecoder::Reset() noexcept
{
    if (implementation_)
    {
        implementation_->acceptedCount = 0;
        implementation_->metricsValid = false;
        implementation_->geometryCache = {};
        implementation_->calibration = {};
    }
}

ExperimentalPam4Observation ExperimentalPam4CpuDecoder::Decode(const LumaView& view, const UnifiedExpectedFrameIdentity& expected) noexcept
{
    ExperimentalPam4Observation observation;
    if (!implementation_)
    {
        return observation;
    }
    auto& state = *implementation_;
    const auto previousGeometry = state.geometryCache;
    Reset();
    if (ValidateLumaView(view) != LocalDesktopErasureReason::None ||
        (view.pixelFormat != LumaPixelFormat::Gray8 && view.pixelFormat != LumaPixelFormat::Bgra8))
    {
        return observation;
    }
    observation.inputValid = true;
    if (!IsPam4Pair(expected.visualProfileId, expected.visualLayoutVersion))
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::IdentityConflict;
        return observation;
    }
    const LocalDesktopBootstrapBinding binding{expected.visualProfileId, expected.visualLayoutVersion};
    const auto* const hint = previousGeometry.scaleX > 0 && previousGeometry.scaleY > 0 ? &previousGeometry : nullptr;
    observation.bootstrap = DecodeLocalDesktopBootstrap(view, binding, {}, hint, nullptr);
    if (!observation.bootstrap.IsAccepted())
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::BootstrapFailure;
        return observation;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (!parsed || !IsPam4Pair(parsed.Value().visualProfileId, parsed.Value().visualLayoutVersion) ||
        (expected.requireSessionTag && expected.sessionTag != parsed.Value().sessionTag) ||
        (expected.requireFrameSequence && expected.frameSequence != parsed.Value().frameSequence))
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::IdentityConflict;
        return observation;
    }
    observation.bootstrapRecord = parsed.Value();
    const auto& geometry = observation.bootstrap.geometry;
    // No hidden crop/clamp: the full canonical canvas must fit in the ROI.
    // Small refinement noise at an exact edge uses the existing convergence
    // tolerance, without moving or snapping the measured sampling geometry.
    constexpr double edgeTolerance = kLocalDesktopGeometryRefinementConvergencePixels;
    if (geometry.scaleX < 0.5 || geometry.scaleY < 0.5 || geometry.scaleX > 2 || geometry.scaleY > 2 ||
        geometry.originX < -edgeTolerance || geometry.originY < -edgeTolerance ||
        geometry.originX + geometry.scaleX * 1920 > view.width + edgeTolerance ||
        geometry.originY + geometry.scaleY * 1080 > view.height + edgeTolerance)
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::CanvasClipped;
        return observation;
    }
    if (!Freshness(view, observation.bootstrap, observation.freshness))
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::FreshnessFailure;
        return observation;
    }
    detail::LumaReader reader(view, kSampleWorkLimit, true);
    ExperimentalPam4Calibration calibration;
    if (!Calibrate(reader, view, geometry, calibration))
    {
        observation.frameErasure = ExperimentalPam4FrameErasure::CalibrationFailure;
        return observation;
    }
    for (std::uint32_t cell = 0; cell < kExperimentalPam4CellCount; cell++)
    {
        const auto site = detail::kExperimentalPam4CellSites[cell];
        double sample = 0;
        if (!Sample(reader, view, geometry, site % 480 * 4.0 + 2, site / 480 * 4.0 + 2, sample))
        {
            observation.frameErasure = ExperimentalPam4FrameErasure::SamplingFailure;
            return observation;
        }
        std::array<double, 4> distances{};
        for (std::size_t level = 0; level < 4; level++)
        {
            const double delta = (sample - calibration.dataCentroids[level]) / 127.5;
            distances[level] = delta * delta;
        }
        state.physicalMetrics[cell * 2] = Quantize(((std::min)(distances[1], distances[3]) - (std::min)(distances[0], distances[2])) * 16384.0);
        state.physicalMetrics[cell * 2 + 1] = Quantize(((std::min)(distances[2], distances[3]) - (std::min)(distances[0], distances[1])) * 16384.0);
    }
    for (std::uint32_t bit = 0; bit < kExperimentalPam4MetricCount; bit++)
    {
        state.logicalMetrics[bit] = state.physicalMetrics[detail::kExperimentalPam4BitPositions[bit]];
    }
    state.metricsValid = true;
    state.calibration = calibration;
    state.geometryCache = geometry;
    observation.frameErasure = ExperimentalPam4FrameErasure::None;
    const pbinnerfec::InnerFecDecodeOptions options{12, 1, 2048, 1, 1};
    for (std::uint32_t slot = 0; slot < kExperimentalPam4CodewordCount; slot++)
    {
        const bool control = slot == 0;
        auto& slotObservation = observation.slots[slot];
        slotObservation.lane = UnifiedLane::BaseLuma;
        slotObservation.kind = control ? UnifiedSlotKind::Control : UnifiedSlotKind::Transport;
        auto& decoder = control ? state.controlDecoder : state.dataDecoder;
        const auto decoded = decoder.Decode(std::span(state.logicalMetrics).subspan(static_cast<std::size_t>(slot) * kExperimentalPam4CodewordBits,
            kExperimentalPam4CodewordBits), options, state.decoded);
        if (!decoded)
        {
            slotObservation.rejection = UnifiedSlotRejection::InnerFecFailure;
            slotObservation.iterationsUsed = decoded.Error().code == pbinnerfec::InnerFecErrorCode::SyndromeFailure && decoded.Error().detail <= options.maxIterations ?
                static_cast<std::uint32_t>(decoded.Error().detail) : 0;
            continue;
        }
        slotObservation.fecValid = true;
        slotObservation.iterationsUsed = decoded.Value().iterationsUsed;
        const auto information = std::span<const std::byte>(state.decoded).first(control ? kExperimentalPam4ControlInformationBytes : kExperimentalPam4DataInformationBytes);
        const auto extracted = control ? pbprotocol::ExtractControlRecordFromInfoBlock(information) : pbprotocol::ExtractTransportBlockFromInfoBlock(information);
        if (!extracted)
        {
            slotObservation.rejection = ProtocolRejection(extracted.Error().code, control);
            continue;
        }
        slotObservation.paddingValid = true;
        slotObservation.crcValid = true;
        bool identityValid = false;
        if (control)
        {
            const auto record = pbprotocol::ParseControlRecord(extracted.Value());
            identityValid = record && record.Value().sessionTag == observation.bootstrapRecord.sessionTag;
        }
        else
        {
            const auto record = pbprotocol::ParseTransportBlock(extracted.Value());
            identityValid = record && record.Value().header.sessionTag == observation.bootstrapRecord.sessionTag;
        }
        if (!identityValid)
        {
            slotObservation.rejection = UnifiedSlotRejection::IdentityFailure;
            continue;
        }
        slotObservation.identityValid = true;
        slotObservation.rejection = UnifiedSlotRejection::None;
        slotObservation.accepted = true;
        slotObservation.acceptedBytes = static_cast<std::uint32_t>(extracted.Value().size());
        auto& block = state.accepted[state.acceptedCount];
        block.codewordSlot = static_cast<std::uint8_t>(slot);
        block.kind = slotObservation.kind;
        block.size = slotObservation.acceptedBytes;
        std::copy(extracted.Value().begin(), extracted.Value().end(), block.bytes.begin());
        state.acceptedCount++;
        observation.acceptedControlRecords += static_cast<std::uint32_t>(control);
        observation.acceptedTransportBlocks += static_cast<std::uint32_t>(!control);
    }
    observation.acceptedBlocks = state.acceptedCount;
    auto& metrics = observation.lumaMetrics;
    metrics.available = true;
    metrics.samples = static_cast<std::uint32_t>(state.logicalMetrics.size());
    metrics.minimumAbsoluteMetric = 32767;
    for (const auto value : state.logicalMetrics)
    {
        const auto magnitude = static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(value)));
        metrics.minimumAbsoluteMetric = (std::min)(metrics.minimumAbsoluteMetric, magnitude);
        metrics.absoluteMetricSum += magnitude;
        metrics.zeroMetrics += static_cast<std::uint32_t>(value == 0);
        metrics.erasedMetrics += static_cast<std::uint32_t>(value == 0);
    }
    return observation;
}

std::span<const UnifiedAcceptedBlock> ExperimentalPam4CpuDecoder::GetAcceptedBlocks() const noexcept
{
    return implementation_ ? std::span<const UnifiedAcceptedBlock>(implementation_->accepted).first(implementation_->acceptedCount) : std::span<const UnifiedAcceptedBlock>{};
}

std::span<const std::int16_t> ExperimentalPam4CpuDecoder::GetSoftMetrics() const noexcept
{
    return implementation_ && implementation_->metricsValid ? std::span<const std::int16_t>(implementation_->logicalMetrics) : std::span<const std::int16_t>{};
}

ExperimentalPam4Calibration ExperimentalPam4CpuDecoder::GetCalibration() const noexcept
{
    return implementation_ ? implementation_->calibration : ExperimentalPam4Calibration{};
}

}
