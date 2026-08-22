#include "pbprotocol/descriptor_binding.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::uint64_t kDefaultSegmentCount = 10000;
constexpr std::uint64_t kMaximumSegmentCount = 65536;

[[nodiscard]] bool ParseSegmentCount(
    const std::string_view text,
    std::uint64_t& segmentCount) noexcept
{
    const auto parseResult = std::from_chars(
        text.data(),
        text.data() + text.size(),
        segmentCount);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size() &&
        segmentCount > 0 &&
        segmentCount <= kMaximumSegmentCount;
}

[[nodiscard]] pbprotocol::SessionId MakeSessionId() noexcept
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size(); byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(byteIndex);
    }
    return sessionId;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeSegmentDescriptor(
    const pbprotocol::SessionTag sessionTag,
    const std::uint64_t segmentOrdinal) noexcept
{
    const pbprotocol::RawDigest rawDigest{};
    return pbprotocol::SegmentDescriptor{
        sessionTag,
        segmentOrdinal,
        segmentOrdinal,
        1,
        1,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat,
        1,
        rawDigest,
        pbprotocol::EncodedDigest{rawDigest.bytes},
        std::nullopt};
}

} // namespace

int main(const int argumentCount, char* arguments[])
{
    std::uint64_t segmentCount = kDefaultSegmentCount;
    if (argumentCount > 2 ||
        (argumentCount == 2 &&
         !ParseSegmentCount(arguments[1], segmentCount)))
    {
        std::cerr << "usage: PBProtocolDescriptorStateBenchmark "
                     "[segment-count:1..65536]\n";
        return 2;
    }

    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxSegmentCount = segmentCount;
    resourcePolicy.maxRawSegmentBytes = 1;
    const pbprotocol::SessionDescriptor sessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        MakeSessionId(),
        segmentCount,
        segmentCount,
        pbprotocol::DigestAlgorithm::Blake3_256};

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    if (!stateResult)
    {
        std::cerr << "BENCHMARK_SETUP_FAILED error="
                  << static_cast<unsigned int>(stateResult.Error().code)
                  << " offset=" << stateResult.Error().offset << '\n';
        return 1;
    }
    auto state = std::move(stateResult).Value();
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);

    const auto bindStart = std::chrono::steady_clock::now();
    for (std::uint64_t segmentOrdinal = 0;
         segmentOrdinal < segmentCount;
        segmentOrdinal++)
    {
        const auto bindResult = state.BindSegmentDescriptor(
            MakeSegmentDescriptor(sessionTag, segmentOrdinal));
        if (!bindResult)
        {
            std::cerr << "BENCHMARK_BIND_FAILED ordinal=" << segmentOrdinal
                      << " error="
                      << static_cast<unsigned int>(bindResult.Error().code)
                      << " offset=" << bindResult.Error().offset << '\n';
            return 1;
        }
    }
    const auto bindEnd = std::chrono::steady_clock::now();

    const auto validationStart = std::chrono::steady_clock::now();
    const pbprotocol::ProtocolStatus validationStatus =
        state.ValidateCompleteSegmentMap();
    const auto validationEnd = std::chrono::steady_clock::now();
    if (!validationStatus)
    {
        std::cerr << "BENCHMARK_VALIDATION_FAILED error="
                  << static_cast<unsigned int>(validationStatus.Error().code)
                  << " offset=" << validationStatus.Error().offset << '\n';
        return 1;
    }

    const double bindSeconds =
        std::chrono::duration<double>(bindEnd - bindStart).count();
    const double validationSeconds =
        std::chrono::duration<double>(
            validationEnd - validationStart).count();
    const double descriptorsPerSecond = bindSeconds == 0.0
        ? std::numeric_limits<double>::infinity()
        : static_cast<double>(segmentCount) / bindSeconds;

    std::cout << std::fixed << std::setprecision(6)
              << "BENCHMARK_COMPLETED\n"
              << "segment_count=" << segmentCount << '\n'
              << "bound_segment_count=" << state.BoundSegmentCount() << '\n'
              << "descriptor_state_bytes="
              << state.DescriptorStateBytesInUse() << '\n'
              << "bind_seconds=" << bindSeconds << '\n'
              << "bind_descriptors_per_second=" << descriptorsPerSecond << '\n'
              << "map_validation_seconds=" << validationSeconds << '\n';
    return 0;
}
