#pragma once

#include "pbouterfec/outer_fec_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstdint>
#include <memory>

namespace pbouterfec
{

class DirectRepeatDecoder;
class WirehairV2Decoder;

namespace detail
{
struct OuterFecDecoderResourceState;
}

namespace test
{
class DecoderTestAccess;
}

// Owns the receiver-wide Outer FEC decoder admission state. Wirehair and
// DirectRepeat decoders must share one manager so neither mode can bypass the
// active-decoder or aggregate-memory caps. Reservations keep the underlying
// state alive after the manager object is destroyed.
//
// Decoder admission and the counters may run concurrently while this manager
// object is stable. Its owner must stop new admissions before moving or
// destroying it.
class OuterFecDecoderResourceManager
{
public:
    OuterFecDecoderResourceManager(
        const OuterFecDecoderResourceManager&) = delete;
    OuterFecDecoderResourceManager& operator=(
        const OuterFecDecoderResourceManager&) = delete;
    OuterFecDecoderResourceManager(
        OuterFecDecoderResourceManager&& other) noexcept;
    OuterFecDecoderResourceManager& operator=(
        OuterFecDecoderResourceManager&& other) noexcept;
    ~OuterFecDecoderResourceManager();

    [[nodiscard]] static OuterFecResult<OuterFecDecoderResourceManager>
    Create(const pbprotocol::ReceiverResourcePolicy& resourcePolicy);

    [[nodiscard]] std::uint64_t GetActiveDecoderCount() const;
    [[nodiscard]] std::uint64_t GetReservedDecoderBytes() const;

    // Cumulative count of admission attempts rejected by any decoder quota:
    // the per-mode dimension checks (encoded size, block width, DirectRepeat
    // block count), estimate overflow, and the shared active-decoder /
    // per-decoder-bytes / aggregate-bytes reservation caps. Every rejection is
    // counted exactly once. Telemetry only: a relaxed atomic load that never
    // blocks admission.
    [[nodiscard]] std::uint64_t GetQuotaExceededCount() const noexcept;

private:
    friend class DirectRepeatDecoder;
    friend class WirehairV2Decoder;
    friend class test::DecoderTestAccess;

    OuterFecDecoderResourceManager() noexcept = default;

    std::shared_ptr<detail::OuterFecDecoderResourceState> state_;
};

} // namespace pbouterfec
