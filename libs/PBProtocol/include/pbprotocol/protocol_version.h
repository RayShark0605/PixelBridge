#pragma once

#include "pbprotocol/protocol_result.h"

#include <cstddef>
#include <cstdint>

namespace pbprotocol {

// Wire-level protocol version.
// Compatibility is interpreted through (major, minor):
//   - different major -> reject parsing
//   - higher minor    -> skip unknown optional fields only
// Reserved bits and bytes must be zero in the current version.
struct ProtocolVersion
{
    std::uint16_t major;
    std::uint16_t minor;

    bool operator==(const ProtocolVersion&) const = default;
};

enum class ProtocolVersionClassification : std::uint8_t
{
    CurrentOrOlderMinor,
    NewerMinorRequiresOptionalValidation
};

// Returns the protocol version implemented by this build.
ProtocolVersion GetProtocolVersion();

// This classifies only the Major/Minor pair. A newer Minor version is not
// accepted until the containing record proves that every unknown field and
// feature is optional and length-delimited according to that record's rules.
[[nodiscard]] ProtocolResult<ProtocolVersionClassification> ClassifyProtocolVersion(
    ProtocolVersion receivedVersion,
    std::size_t fieldOffset = 0);

// Explicit comparison form for versioned protocol components and boundary
// tests. Production callers normally use the overload above.
[[nodiscard]] ProtocolResult<ProtocolVersionClassification> ClassifyProtocolVersionAgainst(
    ProtocolVersion receivedVersion,
    ProtocolVersion supportedVersion,
    std::size_t fieldOffset = 0);

} // namespace pbprotocol
