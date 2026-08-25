#pragma once
// PBProtocolDump core: deterministic record inspector.
//
// Dumps one PixelBridge wire record as a deterministic field table
// (offset / size / field name / value), recomputes every CRC field and
// reports match/mismatch, and runs the authoritative parser to report the
// parse status (error code name + byte offset). The output is byte-stable
// (no timestamps) so tests can exact-assert it.
//
// Supported types:
//   bootstrap           44-byte PB-Bootstrap-1 record (magic PBRG)
//   control             30..65,536-byte PB-Control record (magic PBCR);
//                       type-1/2/3 payloads are further parsed as
//                       session-descriptor / segment-descriptor /
//                       final-manifest payloads; a type-2/type-3 record
//                       needs the --session-descriptor context for the
//                       semantic cross-check (SessionTagMismatch and
//                       manifest mismatches are failures)
//   fragment            24..65,559-byte control fragment record
//   session-descriptor  37-byte descriptor payload (no magic)
//   segment-descriptor  110/142-byte descriptor payload (no magic;
//                       needs the session-descriptor context)
//   final-manifest      65-byte manifest payload (no magic; needs the
//                       session-descriptor context)
//   transport           36..65,571-byte Transport block (no magic;
//                       header CRC + length self-consistency probe)
//   wirehair-descriptor 32-byte canonical Wirehair V2 profile (WHV2)
//   pbvm-manifest       275-byte PB-ReferenceRaster-1 manifest (PBVM)
//
// Auto detection dispatches on the 4-byte magic (PBRG / PBCR / PBVM /
// WHV2); magic-less inputs are probed as Transport blocks. An input that
// matches no type is reported as unrecognized.
//
// Field table safety: every field row is only emitted when the full field
// is present in the input, so truncated inputs never read past their end
// (no out-of-bounds access and no thrown std::out_of_range). Truncated
// inputs simply yield a shorter table plus the authoritative parse
// failure (TruncatedInput / CrcMismatch / ...) with the exact offset.
//
// Context semantics (sessionDescriptor span, from --session-descriptor):
//   empty span              -> semantic cross-check "skipped"; a type that
//                              requires context remains unvalidated and the
//                              CLI exits 1 with missing-context diagnostics
//   37-byte valid descriptor -> context "checked"; a tag/manifest
//                              mismatch surfaces through the parse result
//   37-byte invalid         -> context "invalid-session-descriptor" and
//                              the semantic parse is skipped
// The authoritative parse for context-dependent types uses
// GetDefaultReceiverResourcePolicy() (the Phase-0 local receiver policy),
// so descriptors above that policy's resource limits report the policy
// error code rather than being silently accepted.

#include "pbprotocol/protocol_result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pbdump {

struct FieldRow
{
    std::size_t offset = 0;
    std::size_t size = 0;
    std::string name;
    // Hex rendering of the field value (integer fields are rendered as
    // little-endian value hex, fixed width per type; raw fields as raw
    // byte hex).
    std::string value;
};

// One recomputed CRC gate. Transport blocks carry two (header + payload);
// every other supported type carries at most one.
struct CrcGate
{
    std::string fieldName;
    std::size_t offset = 0;
    std::uint32_t stored = 0;
    std::uint32_t recomputed = 0;

    [[nodiscard]] bool Matches() const noexcept
    {
        return stored == recomputed;
    }
};

struct DumpReport
{
    // Detected/forced type name, or "unrecognized".
    std::string type;
    std::size_t totalBytes = 0;
    // Field table (deterministic order; bounds-guarded).
    std::vector<FieldRow> fields;
    // CRC gates (empty when the type has no CRC field or the CRC bytes
    // are absent from a truncated input).
    std::vector<CrcGate> crcGates;
    // Authoritative parse status.
    bool parseOk = false;
    // True when the semantic parse cannot run at all (a context-dependent
    // type without its session-descriptor context). Distinct from a
    // parse failure.
    bool parseSkipped = false;
    pbprotocol::ProtocolErrorCode parseCode =
        pbprotocol::ProtocolErrorCode::None;
    std::size_t parseOffset = 0;
    // Free-form diagnostic for foreign error namespaces (the PBVM manifest
    // maps onto the protocol namespace with the modulation code name
    // preserved here, e.g. "modulation:CrcMismatch").
    std::string parseDetail;
    // Session-descriptor context check state; only meaningful for control
    // records (type 2/3) and the context-dependent payload types.
    // Values: "none", "skipped", "checked", "SessionTagMismatch",
    // "invalid-session-descriptor".
    std::string contextStatus = "none";

    // Uniform byte-attributable failure diagnostic. `actual` may be
    // "<eof>" and `expected` may be "<eof>" for length failures.
    bool hasDiagnostic = false;
    std::string diagnosticSpace = "record";
    std::size_t diagnosticOffset = 0;
    std::string diagnosticExpected;
    std::string diagnosticActual;

    [[nodiscard]] std::string ParseCodeName() const;
};

// Stable name for a protocol error code (append-only enum; unknown values
// render as "Unknown(<n>)" so new codes never break the output contract).
[[nodiscard]] std::string ProtocolErrorCodeName(
    const pbprotocol::ProtocolErrorCode code);

// Lowercase fixed-width hex of an integer value.
[[nodiscard]] std::string HexValue(
    const std::uint64_t value, const std::size_t hexDigits);

// Dumps one record. typeHint: "auto" or an explicit type name (an unknown
// hint renders as unrecognized). sessionDescriptor: optional 37-byte
// session descriptor payload used for the context-dependent semantic
// cross-check (empty span == not provided).
[[nodiscard]] DumpReport DumpRecord(
    const std::span<const std::byte> input,
    const std::string& typeHint,
    const std::span<const std::byte> sessionDescriptor);

// Renders the deterministic stdout for a dump:
//   [type] <name>
//   [size] <n>
//   [field] offset=<d> size=<d> name=<name> value=<hex>
//   [crc] field=<name> offset=<d> stored=0x<8hex>
//         recomputed=0x<8hex> status=<match|mismatch>   (one per gate)
//   [parse] status=<Success|skipped|<CodeName>> offset=<d>
//           [ detail=<...>]
//   [context] status=<...>      (control records only)
//   [diagnostic] space=<record|context> byte_offset=<n>
//                expected=<...> actual=<...>
//   (unrecognized inputs: [detect] unrecognized / [size] <n>)
// Not noexcept: builds strings (allocation).
[[nodiscard]] std::string FormatDump(const DumpReport& report);

} // namespace pbdump
