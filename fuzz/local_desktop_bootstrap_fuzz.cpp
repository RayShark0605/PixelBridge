#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/local_desktop_decode.h"
#include "../libs/PBModulation/src/local_desktop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using Record = std::array<std::byte, 44>;
using Codeword = std::array<std::byte, 76>;
using Erasure = pbmodulation::LocalDesktopErasureReason;
using PixelFormat = pbmodulation::LumaPixelFormat;
using RsError = pbmodulation::detail::BootstrapRsError;

constexpr std::size_t kMaximumInputBytes = 4096;
constexpr std::size_t kGrayWidth = 960;
constexpr std::size_t kGrayHeight = 540;
constexpr std::size_t kGrayBytes = kGrayWidth * kGrayHeight;
constexpr std::uint64_t kMaximumWorkUnits = 24000000;
constexpr std::uint64_t kDefaultIterations = 64;
constexpr std::uint64_t kMaximumIterations = 1000000;
constexpr std::uint64_t kDefaultSeed = 0x50424C4442534631ULL;

static_assert(pbmodulation::kLocalDesktopBootstrapRecordBytes == 44);
static_assert(pbmodulation::kLocalDesktopRsCodewordBytes == 76);
static_assert(pbmodulation::kLocalDesktopCanvasWidth == 1920 && pbmodulation::kLocalDesktopCanvasHeight == 1080);
static_assert(pbmodulation::kLocalDesktopCellPixels == 8);
static_assert(pbmodulation::kLocalDesktopBootstrapRegions[0] == pbmodulation::LocalDesktopRegion{96, 16, 608, 64});
static_assert(pbmodulation::kLocalDesktopBootstrapRegions[1] == pbmodulation::LocalDesktopRegion{1216, 1000, 608, 64});

void Check(const bool condition, const char* const message)
{
    if (!condition)
    {
        // Unlike assert(), the semantic oracle stays active in Release/ASan.
        std::cerr << "LOCAL_DESKTOP_FUZZ_INVARIANT: " << message << '\n';
        std::abort();
    }
}

class InputReader
{
public:
    explicit InputReader(const std::span<const std::byte> bytes) noexcept : bytes_(bytes)
    {
    }

    [[nodiscard]] std::uint8_t Byte() noexcept
    {
        return position_ < bytes_.size() ? std::to_integer<std::uint8_t>(bytes_[position_++]) : 0;
    }

    [[nodiscard]] std::uint64_t Integer(const std::size_t byteCount = 8) noexcept
    {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < byteCount; index++)
        {
            value |= static_cast<std::uint64_t>(Byte()) << (index * 8);
        }
        return value;
    }

    [[nodiscard]] std::span<const std::byte> Remaining() const noexcept
    {
        return bytes_.subspan(position_);
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

void CheckRsDecode(const std::span<const std::byte> received, const std::size_t outputBytes)
{
    std::array<std::byte, 47> storage;
    storage.fill(std::byte{0xA7});
    const auto original = storage;
    Check(outputBytes <= 45, "RS harness output bound");
    const auto output = std::span<std::byte>(storage).subspan(1, outputBytes);
    const auto status = pbmodulation::detail::DecodeBootstrapRs(received, output);
    if (!status)
    {
        Check(storage == original, "failed RS decode modified output or guards");
        if (received.size() != 76)
        {
            Check(status.error == RsError::InvalidInputSize, "RS decode malformed input length classification");
        }
        else if (outputBytes != 44)
        {
            Check(status.error == RsError::InvalidOutputSize, "RS decode malformed output length classification");
        }
        return;
    }
    Check(received.size() == 76 && outputBytes == 44, "RS accepted a malformed length");
    Check(storage.front() == original.front() && storage[45] == original[45] && storage[46] == original[46], "RS decode guard overwritten");
    Codeword encoded{};
    Check(static_cast<bool>(pbmodulation::detail::EncodeBootstrapRs(output, encoded)), "RS accepted record failed re-encode");
    std::uint32_t distance = 0;
    for (std::size_t index = 0; index < encoded.size(); index++)
    {
        distance += encoded[index] != received[index] ? 1u : 0u;
    }
    Check(distance <= 16 && status.correctedSymbols == distance, "RS success outside correction radius or wrong corrected count");
}

void ExerciseRs(InputReader& input)
{
    const std::size_t outputBytes = input.Byte() % 78;
    const std::size_t decodedBytes = input.Byte() % 46;
    const std::size_t errorCount = input.Byte() % 34;
    const std::size_t firstPosition = input.Byte() % 76;
    const auto errorMask = static_cast<std::byte>(1u + input.Byte() % 255u);
    const auto arbitrary = input.Remaining();

    std::array<std::byte, 80> encodedStorage;
    encodedStorage.fill(std::byte{0xB6});
    const auto originalStorage = encodedStorage;
    const auto encodedStatus = pbmodulation::detail::EncodeBootstrapRs(arbitrary, std::span<std::byte>(encodedStorage).subspan(1, outputBytes));
    if (arbitrary.size() != 44 || outputBytes != 76)
    {
        const auto expected = arbitrary.size() != 44 ? RsError::InvalidInputSize : RsError::InvalidOutputSize;
        Check(encodedStatus.error == expected && encodedStorage == originalStorage, "malformed RS encode changed output or wrong error");
    }
    else
    {
        Check(static_cast<bool>(encodedStatus), "RS encode rejected exact lengths");
        Check(std::equal(arbitrary.begin(), arbitrary.end(), encodedStorage.begin() + 1), "RS systematic data changed");
        Check(encodedStorage.front() == originalStorage.front() && encodedStorage[77] == originalStorage[77] &&
              encodedStorage[78] == originalStorage[78] && encodedStorage[79] == originalStorage[79], "RS encode guard overwritten");
    }
    CheckRsDecode(arbitrary, 44);

    Record record{};
    for (auto& value : record)
    {
        value = static_cast<std::byte>(input.Byte());
    }
    Codeword received{};
    Check(static_cast<bool>(pbmodulation::detail::EncodeBootstrapRs(record, received)), "RS bounded positive seed encode");
    CheckRsDecode(received, decodedBytes);
    // 5 and 76 are coprime, so even the above-radius branch mutates distinct
    // byte symbols. More than 16 errors does not imply every word must reject:
    // any success must instead satisfy the independently checked radius.
    for (std::size_t index = 0; index < errorCount; index++)
    {
        received[(firstPosition + index * 5) % 76] ^= errorMask;
    }
    CheckRsDecode(received, 44);
    if (errorCount <= 16)
    {
        Record decoded{};
        const auto status = pbmodulation::detail::DecodeBootstrapRs(received, decoded);
        Check(status && decoded == record && status.correctedSymbols == errorCount, "RS failed known 0..16-symbol correction");
    }
}

std::size_t PixelBytes(const PixelFormat format) noexcept
{
    switch (format)
    {
    case PixelFormat::Gray8: return 1;
    case PixelFormat::Bgra8:
    case PixelFormat::R10G10B10A2: return 4;
    case PixelFormat::Fp16LinearSdr: return 8;
    default: return 0;
    }
}

Erasure ViewOracle(const pbmodulation::LumaView& view) noexcept
{
    const auto pixelBytes = PixelBytes(view.pixelFormat);
    if (pixelBytes == 0)
    {
        return Erasure::UnsupportedFormat;
    }
    // Divide the actual tiny allocation, rather than form the untrusted
    // width*height/stride footprint used by the implementation under test.
    if (view.width == 0 || view.height == 0 || view.pixels.data() == nullptr || view.width > view.pixels.size() / pixelBytes)
    {
        return Erasure::InvalidView;
    }
    const auto rowBytes = static_cast<std::size_t>(view.width) * pixelBytes;
    if (view.rowPitch < rowBytes || view.height - 1u > (view.pixels.size() - rowBytes) / view.rowPitch)
    {
        return Erasure::InvalidView;
    }
    return Erasure::None;
}

double Coordinate(const std::uint8_t selector, const std::uint32_t extent) noexcept
{
    switch (selector % 8)
    {
    case 0: return 0;
    case 1: return static_cast<double>(extent) - 1;
    case 2: return static_cast<double>(extent) / 2 - 0.5;
    case 3: return -0.25;
    case 4: return static_cast<double>(extent);
    case 5: return std::numeric_limits<double>::quiet_NaN();
    case 6: return std::numeric_limits<double>::infinity();
    default: return std::numeric_limits<double>::max();
    }
}

void CheckErased(const pbmodulation::LocalDesktopObservation& observation)
{
    Check(!observation.IsAccepted(), "ambiguous or invalid pixels were accepted");
    Check(observation.canonical44 == Record{} && observation.quality == 0, "erasure published accepted canonical data or quality");
    Check(observation.workUnits <= kMaximumWorkUnits, "decoder exceeded fixed work budget");
}

void ExerciseSmallView(InputReader& input)
{
    const auto variant = input.Byte() % 8;
    const auto widthSelector = input.Byte();
    const auto heightSelector = input.Byte();
    const auto formatSelector = input.Byte();
    const auto strideSelector = input.Byte();
    const auto xSelector = input.Byte();
    const auto ySelector = input.Byte();
    std::array<std::byte, kMaximumInputBytes> storage{};
    for (auto& value : storage)
    {
        value = static_cast<std::byte>(input.Byte());
    }
    const auto original = storage;
    pbmodulation::LumaView view{storage, 1u + widthSelector % 16u, 1u + heightSelector % 16u, 0,
                               static_cast<PixelFormat>(formatSelector % 4)};
    const auto rowBytes = static_cast<std::size_t>(view.width) * PixelBytes(view.pixelFormat);
    view.rowPitch = rowBytes + strideSelector % 8;
    const auto footprint = static_cast<std::size_t>(view.height - 1u) * view.rowPitch + rowBytes;
    Check(footprint <= storage.size(), "small-view fixture bound");
    view.pixels = std::span<const std::byte>(storage).first(footprint);
    switch (variant)
    {
    case 0: break;
    case 1: view.width = 0; break;
    case 2: view.height = 0; break;
    case 3: view.rowPitch = rowBytes - 1; break;
    case 4: view.pixels = view.pixels.first(footprint - 1); break;
    case 5:
        view.width = std::numeric_limits<std::uint32_t>::max();
        view.height = std::numeric_limits<std::uint32_t>::max();
        view.rowPitch = std::numeric_limits<std::size_t>::max();
        break;
    case 6: view.pixelFormat = static_cast<PixelFormat>(255); break;
    default:
        view.width = widthSelector % 65;
        view.height = heightSelector % 65;
        view.rowPitch = static_cast<std::size_t>(strideSelector) * 2;
        view.pixels = std::span<const std::byte>(storage).first((static_cast<std::size_t>(xSelector) * 16 + ySelector) % (storage.size() + 1));
        break;
    }
    const auto expected = ViewOracle(view);
    Check(pbmodulation::ValidateLumaView(view) == expected, "luma-view footprint/format oracle mismatch");
    const double x = Coordinate(xSelector, view.width);
    const double y = Coordinate(ySelector, view.height);
    constexpr double sentinel = -98765.25;
    double output = sentinel;
    const auto sampleStatus = pbmodulation::SampleLuma(view, x, y, output);
    if (expected == Erasure::None)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            Check(sampleStatus == Erasure::NonFinitePixel, "luma nonfinite coordinate classification");
        }
        else if (x < 0 || y < 0 || x > static_cast<double>(view.width) - 1 || y > static_cast<double>(view.height) - 1)
        {
            Check(sampleStatus == Erasure::SampleOutOfBounds, "luma out-of-bounds coordinate classification");
        }
        else if (view.pixelFormat != PixelFormat::Fp16LinearSdr)
        {
            Check(sampleStatus == Erasure::None, "finite integer-format sample unexpectedly rejected");
        }
        else
        {
            Check(sampleStatus == Erasure::None || sampleStatus == Erasure::NonFinitePixel || sampleStatus == Erasure::InvalidPixelValue,
                  "bounded FP16 sample returned an unrelated failure");
        }
    }
    if (sampleStatus == Erasure::None)
    {
        Check(expected == Erasure::None && std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 &&
              x <= static_cast<double>(view.width) - 1 && y <= static_cast<double>(view.height) - 1, "luma sample accepted invalid coordinates/view");
        Check(std::isfinite(output) && output >= -1.0e-9 && output <= 255.0 + 1.0e-9, "luma sample outside finite SDR range");
        if (view.pixelFormat == PixelFormat::Gray8)
        {
            const auto left = static_cast<std::size_t>(std::floor(x));
            const auto top = static_cast<std::size_t>(std::floor(y));
            const auto right = static_cast<std::size_t>(std::ceil(x));
            const auto bottom = static_cast<std::size_t>(std::ceil(y));
            const double horizontal = x - static_cast<double>(left);
            const double vertical = y - static_cast<double>(top);
            const double topLeft = std::to_integer<unsigned>(view.pixels[top * view.rowPitch + left]);
            const double topRight = std::to_integer<unsigned>(view.pixels[top * view.rowPitch + right]);
            const double bottomLeft = std::to_integer<unsigned>(view.pixels[bottom * view.rowPitch + left]);
            const double bottomRight = std::to_integer<unsigned>(view.pixels[bottom * view.rowPitch + right]);
            const double interpolated = (1 - horizontal) * (1 - vertical) * topLeft + horizontal * (1 - vertical) * topRight +
                                        (1 - horizontal) * vertical * bottomLeft + horizontal * vertical * bottomRight;
            Check(std::abs(output - interpolated) <= 1.0e-9, "Gray8 sample ignored real pitch or bilinear position");
        }
    }
    else
    {
        Check(output == sentinel, "failed luma sample changed output");
        if (expected != Erasure::None)
        {
            Check(sampleStatus == expected, "luma sample bypassed view validation");
        }
    }
    const auto observation = pbmodulation::DecodeLocalDesktopBootstrap(view);
    CheckErased(observation); // All valid shapes here are smaller than one 0.5x canvas.
    if (expected != Erasure::None)
    {
        Check(observation.erasure == expected, "visual decode bypassed view validation");
    }
    Check(storage == original, "borrowed luma pixels modified");

    constexpr std::array<std::uint16_t, 5> invalidHalf{0x7C00, 0xFC00, 0x7E01, 0xBC00, 0x4000};
    const auto bits = invalidHalf[formatSelector % invalidHalf.size()];
    const std::size_t component = strideSelector % 4;
    std::array<std::byte, 8> halfPixel{};
    halfPixel[7] = std::byte{0x3C};
    halfPixel[component * 2] = static_cast<std::byte>(bits & 255u);
    halfPixel[component * 2 + 1] = static_cast<std::byte>(bits >> 8);
    output = sentinel;
    const auto halfStatus = pbmodulation::SampleLuma({halfPixel, 1, 1, 8, PixelFormat::Fp16LinearSdr}, 0, 0, output);
    const auto halfExpected = (bits & 0x7C00u) == 0x7C00u ? Erasure::NonFinitePixel : Erasure::InvalidPixelValue;
    Check(halfStatus == halfExpected && output == sentinel, "sampled FP16 nonfinite/out-of-SDR component accepted");
}

Record CanonicalRecord(const std::uint64_t sequence, const std::uint64_t sessionTag, const std::uint32_t controlEpoch) noexcept
{
    // Independent logical fixture: no product serializer/CRC, and no sender
    // metadata is ever supplied to DecodeLocalDesktopBootstrap.
    Record record{};
    constexpr std::array<std::uint8_t, 8> prefix{0x50, 0x42, 0x52, 0x47, 1, 1, 0, 2};
    for (std::size_t index = 0; index < prefix.size(); index++)
    {
        record[index] = static_cast<std::byte>(prefix[index]);
    }
    const std::array<std::uint64_t, 3> fields{0x50424C4442533031ULL, sessionTag, sequence};
    for (std::size_t field = 0; field < fields.size(); field++)
    {
        for (std::size_t index = 0; index < 8; index++)
        {
            record[8 + field * 8 + index] = static_cast<std::byte>((fields[field] >> (index * 8)) & 255u);
        }
    }
    for (std::size_t index = 0; index < 4; index++)
    {
        record[32 + index] = static_cast<std::byte>((controlEpoch >> (index * 8)) & 255u);
    }
    std::uint32_t checksum = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < 40; index++)
    {
        checksum ^= std::to_integer<std::uint32_t>(record[index]);
        for (unsigned bit = 0; bit < 8; bit++)
        {
            checksum = (checksum >> 1) ^ ((checksum & 1u) != 0 ? 0x82F63B78u : 0u);
        }
    }
    checksum = ~checksum;
    for (std::size_t index = 0; index < 4; index++)
    {
        record[40 + index] = static_cast<std::byte>((checksum >> (index * 8)) & 255u);
    }
    return record;
}

void RenderHalfGray(const Record& record, const std::span<std::byte> bgra, const std::span<std::byte> gray)
{
    Check(bgra.size() == pbmodulation::kLocalDesktopFrameBgraBytes && gray.size() == kGrayBytes, "fixed renderer allocation bound");
    Check(static_cast<bool>(pbmodulation::EncodeLocalDesktopBootstrapFrame(record, bgra)), "valid canonical fixture failed visual encode");
    for (std::size_t row = 0; row < kGrayHeight; row++)
    {
        for (std::size_t column = 0; column < kGrayWidth; column++)
        {
            const std::size_t source = (row * 2 * 1920 + column * 2) * 4;
            Check(bgra[source] == bgra[source + 1] && bgra[source] == bgra[source + 2] && bgra[source + 3] == std::byte{255},
                  "visual encoder stopped producing opaque luma-only pixels");
            gray[row * kGrayWidth + column] = bgra[source];
        }
    }
}

void CorruptCopy(const std::span<std::byte> gray, const std::size_t copyIndex, const std::size_t errorCount,
                 const std::size_t firstPosition, const std::uint8_t mask)
{
    constexpr std::array<std::array<std::size_t, 2>, 2> origins{{{48, 8}, {608, 500}}};
    Check(copyIndex < origins.size() && errorCount <= 16 && firstPosition < 76 && mask != 0, "symbol corruption fixture bound");
    std::array<bool, 76> touched{};
    const auto& origin = origins[copyIndex];
    const std::size_t step = copyIndex == 0 ? 5 : 7;
    for (std::size_t symbol = 0; symbol < errorCount; symbol++)
    {
        const std::size_t position = (firstPosition + symbol * step) % 76;
        Check(!touched[position], "symbol corruption selected a duplicate position");
        touched[position] = true;
        for (std::size_t bit = 0; bit < 8; bit++)
        {
            if ((mask & (1u << bit)) == 0)
            {
                continue;
            }
            const std::size_t cell = position * 8 + bit;
            const std::size_t cellLeft = origin[0] + (cell % 76) * 4;
            const std::size_t cellTop = origin[1] + (cell / 76) * 4;
            for (std::size_t row = 0; row < 4; row++)
            {
                for (std::size_t column = 0; column < 4; column++)
                {
                    auto& pixel = gray[(cellTop + row) * kGrayWidth + cellLeft + column];
                    Check(pixel == std::byte{32} || pixel == std::byte{224}, "hard corruption touched a nonbinary bootstrap pixel");
                    pixel = pixel == std::byte{32} ? std::byte{224} : std::byte{32};
                }
            }
        }
    }
}

void CheckCopy(const pbmodulation::LocalDesktopCopyObservation& copy, const Record& expected)
{
    Check(copy.fecDecoded && copy.crcValid && copy.recordValid && copy.canonical44 == expected, "copy did not independently recover its complete canonical44");
}

void ExerciseStructured(InputReader& input)
{
    const auto variant = input.Byte() % 4;
    const std::size_t errorCountA = input.Byte() % 17;
    const std::size_t errorCountB = input.Byte() % 17;
    const std::size_t firstPositionA = input.Byte() % 76;
    const std::size_t firstPositionB = input.Byte() % 76;
    const auto maskA = static_cast<std::uint8_t>(1u + input.Byte() % 255u);
    const auto maskB = static_cast<std::uint8_t>(1u + input.Byte() % 255u);
    const auto direction = input.Byte();
    const auto sequence = input.Integer();
    const auto sessionTag = input.Integer() | 1u;
    const auto controlEpoch = static_cast<std::uint32_t>(input.Integer(4));
    const auto record = CanonicalRecord(sequence, sessionTag, controlEpoch);
    const auto otherRecord = CanonicalRecord(sequence ^ 1u, sessionTag, controlEpoch);
    Check(record != otherRecord, "mixed-frame fixture reused one sequence");

    // These are fixed maxima, not input-controlled allocation sizes: at most
    // 8,294,400 BGRA bytes plus two 518,400-byte half-scale gray rasters.
    // Every call owns its buffers; libFuzzer inputs share no mutable cache.
    std::vector<std::byte> bgra(pbmodulation::kLocalDesktopFrameBgraBytes);
    std::vector<std::byte> gray(kGrayBytes);
    RenderHalfGray(record, bgra, gray);
    if (variant == 0)
    {
        CorruptCopy(gray, 0, errorCountA, firstPositionA, maskA);
        CorruptCopy(gray, 1, errorCountB, firstPositionB, maskB);
    }
    else
    {
        std::vector<std::byte> other(kGrayBytes);
        RenderHalfGray(otherRecord, bgra, other);
        const std::size_t cut = (direction & 1u) != 0 ? 360u + direction % 200u : 64u + direction;
        for (std::size_t row = 0; row < kGrayHeight; row++)
        {
            for (std::size_t column = 0; column < kGrayWidth; column++)
            {
                const std::size_t offset = row * kGrayWidth + column;
                if (variant == 2)
                {
                    const auto sum = std::to_integer<unsigned>(gray[offset]) + std::to_integer<unsigned>(other[offset]);
                    gray[offset] = static_cast<std::byte>(sum / 2);
                }
                else
                {
                    const bool replace = variant == 3 ? column >= 608 && column < 912 && row >= 500 && row < 532 :
                                         ((direction & 1u) != 0 ? column >= cut : row >= cut);
                    if (replace)
                    {
                        gray[offset] = other[offset];
                    }
                }
            }
        }
    }
    const auto observation = pbmodulation::DecodeLocalDesktopBootstrap(
        {gray, static_cast<std::uint32_t>(kGrayWidth), static_cast<std::uint32_t>(kGrayHeight), kGrayWidth, PixelFormat::Gray8});
    Check(observation.workUnits <= kMaximumWorkUnits, "structured decoder exceeded fixed work budget");
    if (variant == 0)
    {
        Check(observation.IsAccepted() && observation.canonical44 == record, "hard 0..16-symbol corruption failed complete frame recovery");
        CheckCopy(observation.copies[0], record);
        CheckCopy(observation.copies[1], record);
        Check(observation.copies[0].correctedSymbols == errorCountA && observation.copies[1].correctedSymbols == errorCountB,
              "independent A/B corrected-symbol counts changed");
    }
    else
    {
        CheckErased(observation);
        if (variant != 2)
        {
            Check(observation.erasure == Erasure::BootstrapMismatch, "two intact different-sequence copies did not reach mismatch gate");
            CheckCopy(observation.copies[0], record);
            CheckCopy(observation.copies[1], otherRecord);
        }
    }
}

void ExerciseInput(const std::span<const std::byte> bytes)
{
    if (bytes.size() > kMaximumInputBytes)
    {
        return;
    }
    InputReader input(bytes);
    switch (input.Byte() % 3)
    {
    case 0: ExerciseRs(input); break;
    case 1: ExerciseSmallView(input); break;
    default: ExerciseStructured(input); break;
    }
}

#if !defined(PB_USE_LIBFUZZER)

class SplitMix64
{
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed)
    {
    }

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        // Deliberate modulo-2^64 PRNG arithmetic, never a size/offset calculation.
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

bool ParseUint64(const std::string_view text, std::uint64_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        const auto number = static_cast<std::uint64_t>(digit - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - number) / 10)
        {
            return false;
        }
        value = value * 10 + number;
    }
    output = value;
    return true;
}

int ReplayInputFile(const char* const path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::cerr << "cannot open corpus file: " << path << '\n';
        return 2;
    }
    std::array<char, kMaximumInputBytes + 1> input{};
    file.read(input.data(), static_cast<std::streamsize>(input.size()));
    const auto count = file.gcount();
    if (file.bad() || count < 0 || static_cast<std::size_t>(count) > kMaximumInputBytes || (!file.eof() && file.fail()))
    {
        std::cerr << "corpus input exceeds 4096 bytes or could not be read\n";
        return 2;
    }
    ExerciseInput(std::as_bytes(std::span<const char>(input).first(static_cast<std::size_t>(count))));
    std::cout << "CORPUS_REPLAY_VALIDATED bytes=" << count << '\n';
    return 0;
}

int RunMutations(const std::uint64_t iterations, const std::uint64_t seed)
{
    // Fixed semantic smoke before random mutation. Even an eight-iteration
    // run proves positive decode, maximum correction, tear, blend and no
    // cross-observation state. These are input controls, not cached rasters.
    constexpr std::array<std::array<std::uint8_t, 9>, 8> smoke{{
        {0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 5, 0, 0, 0, 0, 5, 6, 0},
        {2, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 0, 16, 16, 3, 71, 0xA4, 0xC2, 0},
        {2, 1, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 0, 0, 0, 0, 0, 0, 0},
        {2, 3, 0, 0, 0, 0, 0, 0, 0}, {2, 0, 1, 15, 75, 1, 0xFE, 0x80, 0}}};
    for (const auto& item : smoke)
    {
        ExerciseInput(std::as_bytes(std::span(item)));
    }
    SplitMix64 random(seed);
    std::array<std::byte, kMaximumInputBytes> bytes{};
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        const std::size_t requestedSize = 1 + static_cast<std::size_t>(random.Next() % kMaximumInputBytes);
        const std::size_t size = iteration % 3 == 2 && iteration / 3 < 17 ? std::max<std::size_t>(requestedSize, 29) : requestedSize;
        for (std::size_t index = 0; index < size; index++)
        {
            bytes[index] = static_cast<std::byte>(random.Next() & 255u);
        }
        bytes[0] = static_cast<std::byte>(iteration % 3);
        if (iteration % 3 == 2 && iteration / 3 < 17)
        {
            // A 64-iteration run covers all 17 correction counts in each
            // spatial copy, while locations, masks and record bytes mutate.
            bytes[1] = std::byte{0};
            bytes[2] = static_cast<std::byte>(iteration / 3);
            bytes[3] = static_cast<std::byte>(16 - iteration / 3);
        }
        ExerciseInput(std::span<const std::byte>(bytes).first(size));
    }
    std::cout << "LOCAL_DESKTOP_FUZZ_COMPLETED smoke=" << smoke.size() << " iterations=" << iterations << " seed=" << seed << '\n';
    return 0;
}

#endif

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size <= kMaximumInputBytes)
    {
        ExerciseInput(std::as_bytes(std::span(data, size)));
    }
    return 0;
}

#if !defined(PB_USE_LIBFUZZER)

int main(const int argumentCount, char* arguments[])
{
    if (argumentCount == 3 && std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }
    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 3 || (argumentCount > 1 && !ParseUint64(arguments[1], iterations)) ||
        (argumentCount > 2 && !ParseUint64(arguments[2], seed)) || iterations == 0 || iterations > kMaximumIterations)
    {
        std::cerr << "usage: PBModulationLocalDesktopBootstrapFuzz [iterations:1..1000000] [seed:uint64]\n"
                     "       PBModulationLocalDesktopBootstrapFuzz --input <file-at-most-4096-bytes>\n";
        return 2;
    }
    return RunMutations(iterations, seed);
}

#endif
