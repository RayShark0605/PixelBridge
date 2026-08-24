#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbprotocol {

// Receiver-local persistent envelope for resume.state records. This format is
// deliberately NOT part of the wire protocol: it only protects state that a
// receiver writes and later re-reads, so its evolution stays independent from
// bootstrap/control/data plane versions. Layout (little-endian):
//   magic 'PBRS' (4 bytes) | version u8 (=1) | reserved u8 (=0) |
//   payloadLength u64 | payload | CRC32C u32 over [magic .. payload]
constexpr std::size_t kResumeRecordEnvelopeBytes = 18;

// Serializes one resume record into output. Requires at least
// kResumeRecordEnvelopeBytes + payload.size() bytes of capacity; extra
// trailing capacity is left untouched. The CRC covers the header and payload,
// never itself. An empty payload produces a valid 18-byte record.
[[nodiscard]] ProtocolStatus SerializeResumeRecord(
    std::span<const std::byte> payload,
    std::span<std::byte> output);

// Parses and validates one resume record against the receiver resource policy.
// Resume state is untrusted persistent input: full policy validation runs
// first (InvalidResourcePolicy), then magic/version/reserved/length/CRC checks
// fail closed with their specific codes. Both the supplied input length and
// the declared total record length must fit maxResumeBytes
// (ResourceLimitExceeded). The returned vector owns a copy of the payload;
// allocation failure fails closed with ResourceExhausted.
[[nodiscard]] ProtocolResult<std::vector<std::byte>> ParseResumeRecord(
    std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy);

// Whole-file quota gate to run before loading or extending resume.state on
// disk. totalBytes is the full record size (envelope plus payload). Zero is
// allowed; anything above maxResumeBytes fails with ResourceLimitExceeded.
[[nodiscard]] ProtocolStatus ValidateResumeStateBudget(
    std::uint64_t totalBytes,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

} // namespace pbprotocol
