#include "decoder_memory_budget.h"

#include "pbprotocol/descriptor_codec.h"

#include <Windows.h>

#include <algorithm>

namespace pbapp
{
namespace
{

inline constexpr std::uint64_t fixedMemoryPlanningBytes = 512ULL * decoderMemoryMebibyte;

[[nodiscard]] bool IsHostSnapshotValid(const DecoderMemoryHostSnapshot& host) noexcept
{
    return host.available && host.totalPhysicalBytes != 0 && host.availablePhysicalBytes <= host.totalPhysicalBytes;
}

} // namespace

DecoderMemoryBudgetError ValidateDecoderMemoryBudget(const DecoderMemoryBudget& budget) noexcept
{
    if (budget.totalDecoderBytes < 4ULL * decoderMemoryMebibyte || budget.totalDecoderBytes > maximumDecoderMemoryBudgetBytes ||
        budget.perDecoderBytes < decoderMemoryMebibyte || budget.perDecoderBytes > maximumDecoderMemoryBudgetBytes ||
        budget.totalDecoderBytes % decoderMemoryMebibyte != 0 || budget.perDecoderBytes % decoderMemoryMebibyte != 0)
    {
        return DecoderMemoryBudgetError::InvalidRange;
    }
    return budget.perDecoderBytes > budget.totalDecoderBytes ? DecoderMemoryBudgetError::PerDecoderExceedsTotal : DecoderMemoryBudgetError::None;
}

std::uint64_t CalculateDecoderResumeBudgetBytes(const DecoderMemoryBudget& budget) noexcept
{
    return ValidateDecoderMemoryBudget(budget) == DecoderMemoryBudgetError::None ?
        (budget.totalDecoderBytes / 4 / decoderMemoryMebibyte) * decoderMemoryMebibyte : 0;
}

std::uint64_t CalculateDecoderMemoryPlanningBytes(const DecoderMemoryBudget& budget) noexcept
{
    // Validation bounds the sum well below uint64_t and size_t on Windows x64.
    // Four journal-sized allowances cover active/pending payload, document
    // loading/compaction and temporary capacity growth without equating the
    // FEC admission charge with all process allocations.
    return ValidateDecoderMemoryBudget(budget) == DecoderMemoryBudgetError::None ?
        budget.totalDecoderBytes + 4 * CalculateDecoderResumeBudgetBytes(budget) + fixedMemoryPlanningBytes : 0;
}

DecoderMemoryBudgetError ApplyDecoderMemoryBudget(const DecoderMemoryBudget& budget, pbprotocol::ReceiverResourcePolicy& policy) noexcept
{
    const auto validation = ValidateDecoderMemoryBudget(budget);
    if (validation != DecoderMemoryBudgetError::None)
    {
        return validation;
    }
    auto candidate = policy;
    candidate.maxOuterFecDecoderBytes = budget.perDecoderBytes;
    candidate.maxTotalOuterFecDecoderBytes = budget.totalDecoderBytes;
    candidate.maxResumeBytes = CalculateDecoderResumeBudgetBytes(budget);
    if (!pbprotocol::ValidateReceiverResourcePolicy(candidate))
    {
        return DecoderMemoryBudgetError::InvalidResourcePolicy;
    }
    policy = candidate;
    return DecoderMemoryBudgetError::None;
}

DecoderMemoryBudgetError ValidateDecoderMemoryAgainstHost(const DecoderMemoryBudget& budget, const DecoderMemoryHostSnapshot& host) noexcept
{
    const auto validation = ValidateDecoderMemoryBudget(budget);
    if (validation != DecoderMemoryBudgetError::None)
    {
        return validation;
    }
    if (!IsHostSnapshotValid(host))
    {
        return DecoderMemoryBudgetError::HostMemoryUnavailable;
    }
    const std::uint64_t availableBytes = (std::min)(host.availablePhysicalBytes, host.availableCommitBytes);
    const std::uint64_t headroomBytes = (std::max)(fixedMemoryPlanningBytes, availableBytes / 10);
    return availableBytes > headroomBytes && CalculateDecoderMemoryPlanningBytes(budget) <= availableBytes - headroomBytes ?
        DecoderMemoryBudgetError::None : DecoderMemoryBudgetError::InsufficientHostMemory;
}

DecoderMemoryBudgetError RecommendDecoderMemoryBudget(const DecoderMemoryHostSnapshot& host, DecoderMemoryBudget& output) noexcept
{
    if (!IsHostSnapshotValid(host))
    {
        return DecoderMemoryBudgetError::HostMemoryUnavailable;
    }
    const std::uint64_t availableBytes = (std::min)(host.availablePhysicalBytes, host.availableCommitBytes);
    const std::uint64_t headroomBytes = (std::max)(fixedMemoryPlanningBytes, availableBytes / 5);
    const std::uint64_t plannedBytes = availableBytes > headroomBytes ? availableBytes - headroomBytes : 0;
    if (plannedBytes <= fixedMemoryPlanningBytes)
    {
        return DecoderMemoryBudgetError::InsufficientHostMemory;
    }
    DecoderMemoryBudget candidate;
    candidate.totalDecoderBytes = (std::min)(maximumDecoderMemoryBudgetBytes,
        ((plannedBytes - fixedMemoryPlanningBytes) / 2 / decoderMemoryMebibyte) * decoderMemoryMebibyte);
    candidate.perDecoderBytes = (std::min)(candidate.perDecoderBytes, candidate.totalDecoderBytes);
    if (ValidateDecoderMemoryBudget(candidate) != DecoderMemoryBudgetError::None)
    {
        return DecoderMemoryBudgetError::InsufficientHostMemory;
    }
    const auto validation = ValidateDecoderMemoryAgainstHost(candidate, host);
    if (validation != DecoderMemoryBudgetError::None)
    {
        return validation;
    }
    output = candidate;
    return DecoderMemoryBudgetError::None;
}

DecoderMemoryHostSnapshot QueryDecoderMemoryHostSnapshot() noexcept
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == FALSE)
    {
        return {};
    }
    return {true, status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPageFile};
}

const char* DescribeDecoderMemoryBudgetError(const DecoderMemoryBudgetError error) noexcept
{
    switch (error)
    {
    case DecoderMemoryBudgetError::None: return "";
    case DecoderMemoryBudgetError::InvalidRange: return "内存预算必须是有限的整MiB；总预算至少4MiB，单实例至少1MiB，均不得超过1TiB";
    case DecoderMemoryBudgetError::PerDecoderExceedsTotal: return "单实例解码预算不能超过活动解码器总预算";
    case DecoderMemoryBudgetError::InvalidResourcePolicy: return "接收端内存预算与资源策略不一致";
    case DecoderMemoryBudgetError::HostMemoryUnavailable: return "无法读取本机可用物理内存/commit；未启动自定义预算接收";
    case DecoderMemoryBudgetError::InsufficientHostMemory: return "当前可用物理内存或commit不足以容纳该预算及恢复/capture余量，请降低预算或重新获取本机建议";
    }
    return "未知接收内存预算错误";
}

} // namespace pbapp
