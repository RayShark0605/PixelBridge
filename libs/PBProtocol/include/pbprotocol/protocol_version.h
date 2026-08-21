#pragma once

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
};

// Returns the protocol version implemented by this build.
ProtocolVersion GetProtocolVersion();

} // namespace pbprotocol