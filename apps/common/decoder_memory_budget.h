#pragma once

#include "pbprotocol/protocol_types.h"

#include <cstdint>

namespace pbapp
{

inline constexpr std::uint64_t decoderMemoryMebibyte = 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumDecoderMemoryBudgetBytes = 1024ULL * 1024ULL * decoderMemoryMebibyte;

// Receiver-local, explicit settings. These values never enter visual wire or
// sender scheduling; no memory is allocated merely by choosing a budget.
struct DecoderMemoryBudget
{
    std::uint64_t totalDecoderBytes = 1024ULL * decoderMemoryMebibyte;
    std::uint64_t perDecoderBytes = 512ULL * decoderMemoryMebibyte;
    bool operator==(const DecoderMemoryBudget&) const = default;
};

struct DecoderMemoryHostSnapshot
{
    bool available = false;
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
    std::uint64_t availableCommitBytes = 0;
};

enum class DecoderMemoryBudgetError : std::uint8_t
{
    None, InvalidRange, PerDecoderExceedsTotal, InvalidResourcePolicy, HostMemoryUnavailable, InsufficientHostMemory
};

[[nodiscard]] DecoderMemoryBudgetError ValidateDecoderMemoryBudget(const DecoderMemoryBudget& budget) noexcept;
[[nodiscard]] std::uint64_t CalculateDecoderResumeBudgetBytes(const DecoderMemoryBudget& budget) noexcept;
// Includes a conservative planning allowance for resume copies and fixed
// capture/application work; it is not a measurement of process RSS/commit.
[[nodiscard]] std::uint64_t CalculateDecoderMemoryPlanningBytes(const DecoderMemoryBudget& budget) noexcept;
[[nodiscard]] DecoderMemoryBudgetError ApplyDecoderMemoryBudget(const DecoderMemoryBudget& budget,
    pbprotocol::ReceiverResourcePolicy& policy) noexcept;
[[nodiscard]] DecoderMemoryBudgetError ValidateDecoderMemoryAgainstHost(const DecoderMemoryBudget& budget,
    const DecoderMemoryHostSnapshot& host) noexcept;
[[nodiscard]] DecoderMemoryBudgetError RecommendDecoderMemoryBudget(const DecoderMemoryHostSnapshot& host,
    DecoderMemoryBudget& output) noexcept;
[[nodiscard]] DecoderMemoryHostSnapshot QueryDecoderMemoryHostSnapshot() noexcept;
[[nodiscard]] const char* DescribeDecoderMemoryBudgetError(DecoderMemoryBudgetError error) noexcept;

} // namespace pbapp
