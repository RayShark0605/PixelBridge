#include "decoder_test_access.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

namespace
{

struct DeallocationObservation
{
    const pbouterfec::OuterFecDecoderResourceManager* resourceManager = nullptr;
    // MSVC vectors over-allocate >= 4096-byte buffers for SIMD alignment.
    // Observe a smaller multi-block payload so operator new/delete identify
    // the actual owned allocation, not a nonexistent unpadded size request.
    std::size_t payloadBytes = 2053;
    void* payload = nullptr;
    bool failAfterPayload = false;
    bool failNextAllocation = false;
    bool failureInjected = false;
    bool payloadFreed = false;
    std::uint64_t activeDecodersAtFree = 0;
    std::uint64_t reservedBytesAtFree = 0;
    std::array<std::size_t, 8> observedAllocationBytes{};
    std::size_t observedAllocationCount = 0;
};

thread_local DeallocationObservation* activeObservation = nullptr;

class ObservationScope final
{
public:
    explicit ObservationScope(DeallocationObservation& observation) noexcept : previous_(activeObservation)
    {
        activeObservation = &observation;
    }
    ~ObservationScope()
    {
        activeObservation = previous_;
    }
    ObservationScope(const ObservationScope&) = delete;
    ObservationScope& operator=(const ObservationScope&) = delete;
private:
    DeallocationObservation* previous_;
};

[[nodiscard]] void* Allocate(const std::size_t byteCount)
{
    if (activeObservation != nullptr && activeObservation->observedAllocationCount < activeObservation->observedAllocationBytes.size())
    {
        activeObservation->observedAllocationBytes[activeObservation->observedAllocationCount++] = byteCount;
    }
    if (activeObservation != nullptr && activeObservation->failNextAllocation)
    {
        activeObservation->failNextAllocation = false;
        activeObservation->failureInjected = true;
        throw std::bad_alloc{};
    }
    void* const allocation = std::malloc(byteCount == 0 ? 1 : byteCount);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    if (activeObservation != nullptr && activeObservation->payload == nullptr &&
        !activeObservation->payloadFreed && byteCount == activeObservation->payloadBytes)
    {
        activeObservation->payload = allocation;
        activeObservation->failNextAllocation = activeObservation->failAfterPayload;
    }
    return allocation;
}

void Deallocate(void* const allocation) noexcept
{
    if (activeObservation != nullptr && allocation != nullptr && allocation == activeObservation->payload)
    {
        activeObservation->activeDecodersAtFree = activeObservation->resourceManager->GetActiveDecoderCount();
        activeObservation->reservedBytesAtFree = activeObservation->resourceManager->GetReservedDecoderBytes();
        activeObservation->payloadFreed = true;
        activeObservation->payload = nullptr;
    }
    std::free(allocation);
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDescriptor(const std::uint64_t encodedBytes)
{
    pbprotocol::SegmentDescriptor descriptor;
    descriptor.encodedSize = encodedBytes;
    descriptor.rawSize = encodedBytes;
    descriptor.outerBlockBytes = 1024;
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    return descriptor;
}

} // namespace

void* operator new(const std::size_t byteCount)
{
    return Allocate(byteCount);
}

void* operator new[](const std::size_t byteCount)
{
    return Allocate(byteCount);
}

void operator delete(void* const allocation) noexcept
{
    Deallocate(allocation);
}

void operator delete[](void* const allocation) noexcept
{
    Deallocate(allocation);
}

void operator delete(void* const allocation, const std::size_t) noexcept
{
    Deallocate(allocation);
}

void operator delete[](void* const allocation, const std::size_t) noexcept
{
    Deallocate(allocation);
}

TEST_CASE("DirectRepeat keeps its reservation while owned buffers are being freed", "[outerfec][resource][lifetime]")
{
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto managerResult = pbouterfec::OuterFecDecoderResourceManager::Create(policy);
    REQUIRE(managerResult);
    auto manager = std::move(managerResult).Value();
    DeallocationObservation observation;
    observation.resourceManager = &manager;
    const auto descriptor = MakeDescriptor(observation.payloadBytes);
    {
        const ObservationScope scope(observation);
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(descriptor, 1024, manager);
        INFO("raw allocation sizes: " << observation.observedAllocationBytes[0] << ", " << observation.observedAllocationBytes[1] << ", " << observation.observedAllocationBytes[2]);
        REQUIRE(decoderResult);
        REQUIRE(observation.payload != nullptr);
        REQUIRE(manager.GetActiveDecoderCount() == 1);
    }
    REQUIRE(observation.payloadFreed);
    REQUIRE(observation.activeDecodersAtFree == 1);
    REQUIRE(observation.reservedBytesAtFree >= observation.payloadBytes);
    REQUIRE(manager.GetActiveDecoderCount() == 0);
    REQUIRE(manager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("DirectRepeat replacement releases old buffers before old admission credit", "[outerfec][resource][lifetime]")
{
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto managerResult = pbouterfec::OuterFecDecoderResourceManager::Create(policy);
    REQUIRE(managerResult);
    auto manager = std::move(managerResult).Value();
    DeallocationObservation observation;
    observation.resourceManager = &manager;
    {
        const ObservationScope scope(observation);
        auto first = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(MakeDescriptor(observation.payloadBytes), 1024, manager);
        auto second = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(MakeDescriptor(observation.payloadBytes + 12), 1024, manager);
        REQUIRE(first);
        REQUIRE(second);
        const auto reservedBytes = manager.GetReservedDecoderBytes();
        first.Value() = std::move(second).Value();
        REQUIRE(observation.payloadFreed);
        REQUIRE(observation.activeDecodersAtFree == 2);
        REQUIRE(observation.reservedBytesAtFree == reservedBytes);
        REQUIRE(manager.GetActiveDecoderCount() == 1);
    }
    REQUIRE(manager.GetActiveDecoderCount() == 0);
    REQUIRE(manager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("DirectRepeat allocation unwind retains the local reservation until buffer cleanup", "[outerfec][resource][lifetime]")
{
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto managerResult = pbouterfec::OuterFecDecoderResourceManager::Create(policy);
    REQUIRE(managerResult);
    auto manager = std::move(managerResult).Value();
    DeallocationObservation observation;
    observation.resourceManager = &manager;
    observation.failAfterPayload = true;
    {
        const ObservationScope scope(observation);
        const auto result = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(MakeDescriptor(observation.payloadBytes), 1024, manager);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbouterfec::OuterFecErrorCode::OutOfMemory);
    }
    REQUIRE(observation.failureInjected);
    REQUIRE(observation.payloadFreed);
    REQUIRE(observation.activeDecodersAtFree == 1);
    REQUIRE(observation.reservedBytesAtFree >= observation.payloadBytes);
    REQUIRE(manager.GetActiveDecoderCount() == 0);
    REQUIRE(manager.GetReservedDecoderBytes() == 0);
}
