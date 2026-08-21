#include "pbprotocol/protocol_version.h"

namespace pbprotocol {

ProtocolVersion GetProtocolVersion()
{
    // Skeleton baseline constant: the fixed bootstrap and control planes are
    // named PB-Bootstrap-1 and PB-Control-1, so the first protocol major is
    // 1 and no minor revision exists yet. The protocol Golden Vectors lock
    // the final serialized interpretation.
    const ProtocolVersion protocolVersion{1, 0};
    return protocolVersion;
}

} // namespace pbprotocol