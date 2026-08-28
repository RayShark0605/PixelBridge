#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/local_desktop_decode.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbdesktoplevels/reference_channel.h"
#include "../../libs/PBModulation/src/local_desktop_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <span>

namespace
{

enum class AllocationKind : std::size_t
{
    Object, Array, AlignedObject, AlignedArray, Count
};

thread_local bool rejectAllocations = false;
thread_local std::size_t allocationsBeforeFailure = 0;
thread_local std::array<std::uint64_t, static_cast<std::size_t>(AllocationKind::Count)> deniedAllocations{};

bool RejectAllocation(const AllocationKind kind) noexcept
{
    if (!rejectAllocations)
    {
        return false;
    }
    if (allocationsBeforeFailure != 0)
    {
        allocationsBeforeFailure--;
        return false;
    }
    // A successful probe has exactly two positive-control denials per kind.
    // Any additional attempt is a failure, even if a product catches bad_alloc.
    auto& count = deniedAllocations[static_cast<std::size_t>(kind)];
    if (count != std::numeric_limits<std::uint64_t>::max())
    {
        count++;
    }
    return true;
}

void* TryAllocateUnaligned(const std::size_t byteCount, const AllocationKind kind) noexcept
{
    return RejectAllocation(kind) ? nullptr : std::malloc(byteCount == 0 ? 1 : byteCount);
}

void* TryAllocateAligned(const std::size_t byteCount, const std::size_t alignment, const AllocationKind kind) noexcept
{
    return RejectAllocation(kind) ? nullptr : _aligned_malloc(byteCount == 0 ? 1 : byteCount, alignment);
}

void* RequireAllocation(void* const allocation)
{
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

class AllocationRejection
{
public:
    AllocationRejection() noexcept
    {
        rejectAllocations = true;
    }
    ~AllocationRejection()
    {
        rejectAllocations = false;
    }
    AllocationRejection(const AllocationRejection&) = delete;
    AllocationRejection& operator=(const AllocationRejection&) = delete;
};

struct ProbeChecks
{
    std::uint32_t count = 0;
    std::uint32_t failures = 0;
    const char* firstFailure = nullptr;

    void Check(const bool condition, const char* const description) noexcept
    {
        count++;
        if (!condition)
        {
            failures++;
            if (firstFailure == nullptr)
            {
                firstFailure = description;
            }
        }
    }
};

consteval std::uint8_t HexDigit(const char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return static_cast<std::uint8_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return static_cast<std::uint8_t>(digit - 'a' + 10);
    }
    throw "invalid literal oracle digit";
}

template<std::size_t numCharacters>
consteval auto HexBytes(const char (&text)[numCharacters])
{
    static_assert(numCharacters % 2 == 1);
    std::array<std::byte, (numCharacters - 1) / 2> bytes{};
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((HexDigit(text[2 * index]) << 4) | HexDigit(text[2 * index + 1]));
    }
    return bytes;
}

using Record = std::array<std::byte, 44>;
using Codeword = std::array<std::byte, 76>;
using pbmodulation::detail::BootstrapRsError;
using pbmodulation::detail::DecodeBootstrapRs;
using pbmodulation::detail::EncodeBootstrapRs;
using Erasure = pbmodulation::LocalDesktopErasureReason;

// Independently generated Python-oracle bytes already pinned under
// tests/golden/local-desktop-bootstrap. Do not regenerate these using the SUT.
// No fixture loader, serializer, encoder or digest executes before the guard.
constexpr Record canonical = HexBytes("504252470101000231305342444c4250d0ba97d94b20df811817161514131211242322210000000099102656");
constexpr Codeword encodedOracle = HexBytes(
    "504252470101000231305342444c4250d0ba97d94b20df811817161514131211242322210000000099102656"
    "db45fbb1f5eb0076bd065155781f8da00b9e768c6863b6d1a237f0d6e67658da");
constexpr Codeword omittedFirst = HexBytes(
    "504252470101000231305342444c4250d0ba97d94b20df811817161514131211242322210000000099102656"
    "fc32cef3fce71c3cacb6e2e6c6ab694cb20c5d933fdff20c1458f1c930eb739d");
constexpr Codeword omittedLast = HexBytes(
    "504252470101000231305342444c4250d0ba97d94b20df811817161514131211242322210000000099102656"
    "4f147b0de7427e907e2ad78b324504292ea0dc33974b81cf079e4dbd15144957");
constexpr Codeword uncorrectable = HexBytes(
    "504252470101000231305342444c4250d0ba97d94b20df811817161514131211242322210000000099102656"
    "db45fbb1f5eb0076bd065155781f8da130931e312cb2a8d90176d93384447ce1");
constexpr auto rasterDigestOracle = HexBytes("248b075d46be224f982633732960cd13e80be8eea4f7329554179216d5ef293f");
constexpr std::size_t fullRasterBytes = 1920u * 1080u * 4u;
constexpr std::size_t halfRasterBytes = 960u * 540u;
constexpr std::size_t guardBytes = 16;
constexpr std::byte guardValue{0xC9};
static_assert(fullRasterBytes == pbmodulation::kLocalDesktopFrameBgraBytes);
static_assert(canonical.size() == pbmodulation::kLocalDesktopBootstrapRecordBytes);
static_assert(encodedOracle.size() == pbmodulation::kLocalDesktopRsCodewordBytes);

void CheckAllocationControls(ProbeChecks& checks)
{
    const auto before = deniedAllocations;
    for (std::size_t kind = 0; kind < before.size(); kind++)
    {
        bool rejected = false;
        try
        {
            // Explicit replaceable-function calls cannot be elided like a
            // new-expression whose allocated object is never observed.
            void* allocation = nullptr;
            switch (static_cast<AllocationKind>(kind))
            {
            case AllocationKind::Object: allocation = ::operator new(17); ::operator delete(allocation); break;
            case AllocationKind::Array: allocation = ::operator new[](17); ::operator delete[](allocation); break;
            case AllocationKind::AlignedObject: allocation = ::operator new(17, std::align_val_t{64}); ::operator delete(allocation, std::align_val_t{64}); break;
            case AllocationKind::AlignedArray: allocation = ::operator new[](17, std::align_val_t{64}); ::operator delete[](allocation, std::align_val_t{64}); break;
            case AllocationKind::Count: break;
            }
        }
        catch (const std::bad_alloc&)
        {
            // This is the sole expected exception boundary. Product calls are
            // deliberately outside every catch block in this executable.
            rejected = true;
        }
        checks.Check(rejected, "throwing new/new[]/aligned-new positive control was not rejected");

        void* allocation = nullptr;
        switch (static_cast<AllocationKind>(kind))
        {
        case AllocationKind::Object: allocation = ::operator new(17, std::nothrow); break;
        case AllocationKind::Array: allocation = ::operator new[](17, std::nothrow); break;
        case AllocationKind::AlignedObject: allocation = ::operator new(17, std::align_val_t{64}, std::nothrow); break;
        case AllocationKind::AlignedArray: allocation = ::operator new[](17, std::align_val_t{64}, std::nothrow); break;
        case AllocationKind::Count: break;
        }
        checks.Check(allocation == nullptr, "nothrow new/new[]/aligned-new positive control was not rejected");
        switch (static_cast<AllocationKind>(kind))
        {
        case AllocationKind::Object: ::operator delete(allocation); break;
        case AllocationKind::Array: ::operator delete[](allocation); break;
        case AllocationKind::AlignedObject: ::operator delete(allocation, std::align_val_t{64}); break;
        case AllocationKind::AlignedArray: ::operator delete[](allocation, std::align_val_t{64}); break;
        case AllocationKind::Count: break;
        }
        checks.Check(deniedAllocations[kind] == before[kind] + 2, "allocation control did not reach both replacement operators");
    }
}

void CheckRsFailure(ProbeChecks& checks, const Codeword& input, const BootstrapRsError expected, const std::uint32_t position)
{
    std::array<std::byte, 46> output;
    output.fill(std::byte{0xA7});
    const auto before = output;
    const auto status = DecodeBootstrapRs(input, std::span(output).subspan(1, 44));
    checks.Check(status.error == expected && status.errorPosition == position && status.correctedSymbols == 0, "RS failure classification or shortening position changed");
    checks.Check(output == before, "failed RS decode changed output or guard bytes");

    auto overlapping = input;
    const auto original = overlapping;
    const auto aliasStatus = DecodeBootstrapRs(overlapping, std::span(overlapping).subspan(16, 44));
    checks.Check(aliasStatus.error == expected && aliasStatus.errorPosition == position && aliasStatus.correctedSymbols == 0, "overlapping RS failure classification changed");
    checks.Check(overlapping == original, "failed overlapping RS decode changed caller bytes");
}

void CheckRsLengths(ProbeChecks& checks)
{
    std::array<std::byte, 96> storage;
    storage.fill(std::byte{0xAD});
    const auto before = storage;
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{43}, std::size_t{45}})
    {
        const auto status = EncodeBootstrapRs(std::span(storage).first(length), std::span(storage).subspan(1, 76));
        checks.Check(status.error == BootstrapRsError::InvalidInputSize && status.correctedSymbols == 0 && status.errorPosition == 0, "RS encode input-length error changed");
        checks.Check(storage == before, "malformed RS encode input changed output");
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{75}, std::size_t{77}})
    {
        const auto encodeStatus = EncodeBootstrapRs(canonical, std::span(storage).subspan(1, length));
        checks.Check(encodeStatus.error == BootstrapRsError::InvalidOutputSize && encodeStatus.correctedSymbols == 0 && encodeStatus.errorPosition == 0, "RS encode output-length error changed");
        checks.Check(storage == before, "malformed RS encode output changed caller bytes");
        const auto decodeStatus = DecodeBootstrapRs(std::span(storage).first(length), std::span(storage).subspan(1, 44));
        checks.Check(decodeStatus.error == BootstrapRsError::InvalidInputSize && decodeStatus.correctedSymbols == 0 && decodeStatus.errorPosition == 0, "RS decode input-length error changed");
        checks.Check(storage == before, "malformed RS decode input changed output");
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{43}, std::size_t{45}})
    {
        const auto status = DecodeBootstrapRs(encodedOracle, std::span(storage).subspan(1, length));
        checks.Check(status.error == BootstrapRsError::InvalidOutputSize && status.correctedSymbols == 0 && status.errorPosition == 0, "RS decode output-length error changed");
        checks.Check(storage == before, "malformed RS decode output changed caller bytes");
    }
}

void CheckRs(ProbeChecks& checks)
{
    // These are the first SUT calls in the process: not even a successful
    // encoder call or a GF-table warmup is allowed before this guarded decode.
    Record firstDecoded{};
    const auto firstStatus = DecodeBootstrapRs(encodedOracle, firstDecoded);
    checks.Check(firstStatus && firstStatus.correctedSymbols == 0 && firstStatus.errorPosition == 0 && firstDecoded == canonical, "cold RS decode did not match independent literal");
    Codeword firstEncoded{};
    checks.Check(EncodeBootstrapRs(canonical, firstEncoded) && firstEncoded == encodedOracle, "cold RS encode did not match independent literal");

    for (std::uint32_t repeat = 0; repeat < 4; repeat++)
    {
        for (std::uint32_t errors = 0; errors <= 16; errors++)
        {
            auto corrupted = encodedOracle;
            for (std::uint32_t index = 0; index < errors; index++)
            {
                // 5 is coprime to 76; all errors are distinct nonzero symbols,
                // crossing data/parity rather than exercising only one byte.
                const auto position = (repeat * 13u + index * 5u) % 76u;
                const auto magnitude = static_cast<std::byte>(1u + (index * 37u + repeat * 53u) % 255u);
                corrupted[position] ^= magnitude;
            }
            std::array<std::byte, 46> decoded;
            decoded.fill(std::byte{0xB5});
            const auto status = DecodeBootstrapRs(corrupted, std::span(decoded).subspan(1, 44));
            checks.Check(status && status.correctedSymbols == errors && status.errorPosition == 0, "RS 0..16-error correction failed or reported a wrong count");
            checks.Check(std::equal(canonical.begin(), canonical.end(), decoded.begin() + 1), "RS correction changed a canonical literal byte");
            checks.Check(decoded.front() == std::byte{0xB5} && decoded.back() == std::byte{0xB5}, "RS correction overwrote output guards");
            Codeword encoded{};
            checks.Check(EncodeBootstrapRs(canonical, encoded) && encoded == encodedOracle, "repeated RS encoder changed literal codeword");
        }
        CheckRsLengths(checks);
        CheckRsFailure(checks, uncorrectable, BootstrapRsError::LocatorDegree, 0);
        for (const bool additionalError : {false, true})
        {
            auto first = omittedFirst;
            auto last = omittedLast;
            if (additionalError)
            {
                first[75] ^= std::byte{0xE1};
                last[75] ^= std::byte{0xE1};
            }
            CheckRsFailure(checks, first, BootstrapRsError::ShorteningViolation, 0);
            CheckRsFailure(checks, last, BootstrapRsError::ShorteningViolation, 178);
        }
    }
    const Record zeros{};
    const Codeword zeroWord{};
    Codeword encoded{};
    checks.Check(EncodeBootstrapRs(zeros, encoded) && encoded == zeroWord, "opaque all-zero RS encode failed");
    Record decoded;
    decoded.fill(std::byte{0xCD});
    const auto zeroStatus = DecodeBootstrapRs(zeroWord, decoded);
    checks.Check(zeroStatus && zeroStatus.correctedSymbols == 0 && decoded == zeros, "opaque all-zero RS decode failed");
    for (const std::size_t offset : {std::size_t{0}, std::size_t{16}, std::size_t{32}})
    {
        std::array<std::byte, 128> overlapping;
        overlapping.fill(std::byte{0xC5});
        std::copy(canonical.begin(), canonical.end(), overlapping.begin() + static_cast<std::ptrdiff_t>(offset));
        const auto encodeStatus = EncodeBootstrapRs(std::span(overlapping).subspan(offset, 44), std::span(overlapping).first(76));
        checks.Check(encodeStatus && std::equal(encodedOracle.begin(), encodedOracle.end(), overlapping.begin()), "guarded overlapping RS encode failed");
        overlapping[0] ^= std::byte{0x27};
        overlapping[75] ^= std::byte{0xFE};
        const auto decodeStatus = DecodeBootstrapRs(std::span(overlapping).first(76), std::span(overlapping).subspan(offset, 44));
        checks.Check(decodeStatus && decodeStatus.correctedSymbols == 2 &&
                     std::equal(canonical.begin(), canonical.end(), overlapping.begin() + static_cast<std::ptrdiff_t>(offset)) && overlapping.back() == std::byte{0xC5},
                     "guarded overlapping RS correction failed or overwrote its guard");
    }
}

void CheckObservation(ProbeChecks& checks, const pbmodulation::LocalDesktopObservation& observation, const std::uint32_t errors)
{
    checks.Check(observation.IsAccepted() && observation.canonical44 == canonical, "guarded visual decode did not recover the independent canonical44");
    checks.Check(observation.workUnits <= 24000000, "visual decoder exceeded fixed work budget");
    for (const auto& copy : observation.copies)
    {
        checks.Check(copy.fecDecoded && copy.crcValid && copy.recordValid && copy.canonical44 == canonical && copy.correctedSymbols == errors,
                     "guarded A/B decode lost independent CRC/FEC/correction evidence");
    }
}

void CheckRaster(ProbeChecks& checks, const std::span<std::byte> raster, const std::span<std::byte> half)
{
    using pbmodulation::EncodeLocalDesktopBootstrapFrame;
    using pbmodulation::ModulationErrorCode;
    const auto first = EncodeLocalDesktopBootstrapFrame(canonical, raster);
    checks.Check(static_cast<bool>(first), "cold full-raster encoder failed");
    checks.Check(pbprotocol::ComputeBlake3Digest(raster) == rasterDigestOracle, "full raster disagreed with independent pinned digest");
    const pbmodulation::LumaView fullView{raster, 1920, 1080, 1920u * 4u, pbmodulation::LumaPixelFormat::Bgra8};
    CheckObservation(checks, pbmodulation::DecodeLocalDesktopBootstrap(fullView), 0);

    auto invalid = canonical;
    invalid[40] ^= std::byte{1};
    const auto badCrc = EncodeLocalDesktopBootstrapFrame(invalid, raster);
    checks.Check(badCrc.Error() == pbmodulation::ModulationError{ModulationErrorCode::CrcMismatch, 40}, "raster CRC error changed");
    checks.Check(pbprotocol::ComputeBlake3Digest(raster) == rasterDigestOracle, "raster CRC failure modified output");
    const auto shortInput = EncodeLocalDesktopBootstrapFrame(std::span(canonical).first(43), raster);
    checks.Check(shortInput.Error() == pbmodulation::ModulationError{ModulationErrorCode::InvalidInput, 0}, "raster short-input error changed");
    const auto shortOutput = EncodeLocalDesktopBootstrapFrame(canonical, raster.first(raster.size() - 1));
    checks.Check(shortOutput.Error() == pbmodulation::ModulationError{ModulationErrorCode::OutputBufferTooSmall, 0}, "raster short-output error changed");
    checks.Check(pbprotocol::ComputeBlake3Digest(raster) == rasterDigestOracle, "raster size failure modified output");

    for (std::size_t row = 0; row < 540; row++)
    {
        for (std::size_t column = 0; column < 960; column++)
        {
            half[row * 960 + column] = raster[(row * 2 * 1920 + column * 2) * 4];
        }
    }
    const pbmodulation::LumaView halfView{half, 960, 540, 960, pbmodulation::LumaPixelFormat::Gray8};
    CheckObservation(checks, pbmodulation::DecodeLocalDesktopBootstrap(halfView), 0);
    // Hard symbol errors still exercise the full raster locator and both RS
    // decoders without allocating any new fixture or changing timing pixels.
    constexpr std::array<std::array<std::size_t, 2>, 2> copyOrigins{{{48, 8}, {608, 500}}};
    for (std::size_t copy = 0; copy < copyOrigins.size(); copy++)
    {
        for (std::size_t error = 0; error < 16; error++)
        {
            const std::size_t position = (copy * 13 + error * 5) % 76;
            const std::size_t bit = error % 8;
            const std::size_t cell = position * 8 + bit;
            const std::size_t left = copyOrigins[copy][0] + (cell % 76) * 4;
            const std::size_t top = copyOrigins[copy][1] + (cell / 76) * 4;
            for (std::size_t row = 0; row < 4; row++)
            {
                for (std::size_t column = 0; column < 4; column++)
                {
                    auto& pixel = half[(top + row) * 960 + left + column];
                    checks.Check(pixel == std::byte{32} || pixel == std::byte{224}, "correction fixture touched nonbinary pixels");
                    pixel = pixel == std::byte{32} ? std::byte{224} : std::byte{32};
                }
            }
        }
    }
    CheckObservation(checks, pbmodulation::DecodeLocalDesktopBootstrap(halfView), 16);
    checks.Check(EncodeLocalDesktopBootstrapFrame(canonical, raster) && pbprotocol::ComputeBlake3Digest(raster) == rasterDigestOracle,
                 "repeated full-raster encoder changed output");
    CheckObservation(checks, pbmodulation::DecodeLocalDesktopBootstrap(fullView), 0);

    pbmodulation::LocalDesktopDecodePolicy limited;
    limited.maximumWorkUnits = 1;
    const auto exhausted = pbmodulation::DecodeLocalDesktopBootstrap(fullView, limited);
    checks.Check(exhausted.erasure == Erasure::WorkBudgetExceeded && exhausted.workUnits <= 1 && exhausted.canonical44 == Record{} && exhausted.quality == 0,
                 "guarded visual budget failure exposed an accepted result");
    const auto invalidView = pbmodulation::DecodeLocalDesktopBootstrap({raster, 1920, 1080, 1, pbmodulation::LumaPixelFormat::Bgra8});
    checks.Check(invalidView.erasure == Erasure::InvalidView && invalidView.canonical44 == Record{} && invalidView.quality == 0, "guarded malformed view was not erased");

    const std::array<std::byte, 4> gray{std::byte{32}, std::byte{224}, std::byte{224}, std::byte{32}};
    double sample = -1234.0;
    checks.Check(pbmodulation::SampleLuma({gray, 2, 2, 2, pbmodulation::LumaPixelFormat::Gray8}, 0.5, 0.5, sample) == Erasure::None && sample == 128,
                 "guarded local bilinear sample failed");
    sample = -1234.0;
    checks.Check(pbmodulation::SampleLuma({gray, 2, 2, 1, pbmodulation::LumaPixelFormat::Gray8}, 0, 0, sample) == Erasure::InvalidView && sample == -1234.0,
                 "guarded failed local sample modified output");
}

void CheckDesktopLevels(ProbeChecks& checks, const std::span<std::byte> raster)
{
    // Fault every allocation prefix, not only the first allocation. Each failed
    // factory must release partial workspaces; ASan checks the same sweep.
    bool reachedSuccess = false;
    for (std::size_t prefix = 0; prefix < 64 && !reachedSuccess; prefix++)
    {
        {
            const AllocationRejection guard;
            allocationsBeforeFailure = prefix;
            auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
            reachedSuccess = static_cast<bool>(created);
            if (!created)
            {
                checks.Check(created.Error().code == pbmodulation::ModulationErrorCode::MemoryAllocationFailure, "DesktopLevels partial factory allocation did not fail closed");
            }
        }
        allocationsBeforeFailure = 0;
    }
    checks.Check(reachedSuccess, "DesktopLevels allocation-prefix sweep did not reach a fully allocated instance");
    auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    checks.Check(static_cast<bool>(created), "DesktopLevels normal workspace allocation failed");
    if (!created)
    {
        return;
    }
    auto channel = std::move(created).Value();
    const auto data = std::make_unique<std::byte[]>(pbmodulation::kDesktopLevelsMaximumDataBytes);
    const AllocationRejection guard;
    const auto before = deniedAllocations;
    checks.Check(!pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes - 1), "DesktopLevels accepted an under-reserved processor");
    for (const auto profileId : {pbmodulation::kDesktopLevels2ProfileId, pbmodulation::kDesktopLevels4ProfileId})
    {
        pbprotocol::BootstrapRecord record;
        record.visualLayoutVersion = pbmodulation::kDesktopLevelsLayoutVersion;
        record.protocolVersion = pbprotocol::GetProtocolVersion();
        record.visualProfileId = profileId;
        record.sessionTag.value = 0x1122334455667788ULL;
        const auto& profile = *pbmodulation::GetDesktopLevelsProfile(profileId);
        const std::span logicalData(data.get(), profile.dataBytes);
        std::array<std::byte, 44> bytes{};
        checks.Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(record, bytes)), "DesktopLevels guarded Bootstrap serialization failed");
        checks.Check(static_cast<bool>(pbdesktoplevels::GenerateDiagnosticData(bytes, logicalData)), "DesktopLevels guarded Transport/LDPC generation failed");
        checks.Check(static_cast<bool>(pbmodulation::EncodeDesktopLevelsFrame(bytes, logicalData, raster)), "DesktopLevels guarded renderer failed");
        const auto result = channel.Decode({raster, 1920, 1080, 7680, pbmodulation::LumaPixelFormat::Bgra8});
        checks.Check(result.modulation.IsAccepted() && result.evaluation.IsVerified() && result.evaluation.erroneousCodedBits == 0,
            "DesktopLevels guarded end-to-end recovery failed");
    }
    checks.Check(deniedAllocations == before, "DesktopLevels hot path attempted C++ allocation");
}

} // namespace

// Same replaceable-allocation boundary as the existing digest probe, expanded
// to include nonthrowing forms. Placement new does not allocate and is excluded.
// This observes C++ allocation, not direct C malloc or Windows HeapAlloc calls.
void* operator new(const std::size_t byteCount)
{
    return RequireAllocation(TryAllocateUnaligned(byteCount, AllocationKind::Object));
}
void* operator new[](const std::size_t byteCount)
{
    return RequireAllocation(TryAllocateUnaligned(byteCount, AllocationKind::Array));
}
void* operator new(const std::size_t byteCount, const std::align_val_t alignment)
{
    return RequireAllocation(TryAllocateAligned(byteCount, static_cast<std::size_t>(alignment), AllocationKind::AlignedObject));
}
void* operator new[](const std::size_t byteCount, const std::align_val_t alignment)
{
    return RequireAllocation(TryAllocateAligned(byteCount, static_cast<std::size_t>(alignment), AllocationKind::AlignedArray));
}
void* operator new(const std::size_t byteCount, const std::nothrow_t&) noexcept
{
    return TryAllocateUnaligned(byteCount, AllocationKind::Object);
}
void* operator new[](const std::size_t byteCount, const std::nothrow_t&) noexcept
{
    return TryAllocateUnaligned(byteCount, AllocationKind::Array);
}
void* operator new(const std::size_t byteCount, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return TryAllocateAligned(byteCount, static_cast<std::size_t>(alignment), AllocationKind::AlignedObject);
}
void* operator new[](const std::size_t byteCount, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return TryAllocateAligned(byteCount, static_cast<std::size_t>(alignment), AllocationKind::AlignedArray);
}
void operator delete(void* const allocation) noexcept
{
    std::free(allocation);
}
void operator delete[](void* const allocation) noexcept
{
    std::free(allocation);
}
void operator delete(void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}
void operator delete[](void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}
void operator delete(void* const allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
void operator delete[](void* const allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
void operator delete(void* const allocation, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}
void operator delete[](void* const allocation, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}
void operator delete(void* const allocation, const std::size_t, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}
void operator delete[](void* const allocation, const std::size_t, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}
void operator delete(void* const allocation, const std::align_val_t, const std::nothrow_t&) noexcept
{
    _aligned_free(allocation);
}
void operator delete[](void* const allocation, const std::align_val_t, const std::nothrow_t&) noexcept
{
    _aligned_free(allocation);
}

int main()
{
    // Allocate only caller-owned, input-independent raster storage beforehand.
    // There is deliberately no preflight SUT call and no product warmup here.
    const auto rasterStorage = std::make_unique<std::byte[]>(fullRasterBytes + 2 * guardBytes);
    const auto halfStorage = std::make_unique<std::byte[]>(halfRasterBytes + 2 * guardBytes);
    const std::span rasterAllocation(rasterStorage.get(), fullRasterBytes + 2 * guardBytes);
    const std::span halfAllocation(halfStorage.get(), halfRasterBytes + 2 * guardBytes);
    std::fill(rasterAllocation.begin(), rasterAllocation.end(), guardValue);
    std::fill(halfAllocation.begin(), halfAllocation.end(), guardValue);
    ProbeChecks checks;
    {
        const AllocationRejection guard;
        CheckAllocationControls(checks);
        const auto controls = deniedAllocations;
        CheckRs(checks);
        CheckRaster(checks, rasterAllocation.subspan(guardBytes, fullRasterBytes), halfAllocation.subspan(guardBytes, halfRasterBytes));
        checks.Check(deniedAllocations == controls, "SUT attempted C++ allocation under the rejection guard");
        for (const auto allocation : {rasterAllocation, halfAllocation})
        {
            const auto intact = [](const std::byte value)
            {
                return value == guardValue;
            };
            checks.Check(std::ranges::all_of(allocation.first(guardBytes), intact) && std::ranges::all_of(allocation.last(guardBytes), intact),
                         "raster path changed its caller-owned guard bytes");
        }
    }
    CheckDesktopLevels(checks, rasterAllocation.subspan(guardBytes, fullRasterBytes));
    if (checks.failures != 0)
    {
        std::fprintf(stderr, "LOCAL_DESKTOP_NO_ALLOCATION_FAILED checks=%u failures=%u first=%s\n", checks.count, checks.failures, checks.firstFailure);
        return 1;
    }
    // Verify that the guard also restores the normal allocation behavior.
    void* const restored = ::operator new(1, std::nothrow);
    if (restored == nullptr)
    {
        std::fputs("LOCAL_DESKTOP_NO_ALLOCATION_FAILED guard did not restore allocation\n", stderr);
        return 1;
    }
    ::operator delete(restored);
    std::printf("LOCAL_DESKTOP_NO_ALLOCATION_PASS checks=%u positiveControls=8 sutAllocationAttempts=0 coldRs=1 correctedCounts=0..16 raster=full-and-half DesktopLevelsAllocationGate=PASS\n", checks.count);
    return 0;
}
