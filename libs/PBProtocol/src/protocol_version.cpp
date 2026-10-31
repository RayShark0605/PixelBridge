#include "pbprotocol/protocol_version.h"

namespace pbprotocol {

ProtocolVersion GetProtocolVersion()
{
    // Protocol baseline constant: the fixed bootstrap and control planes are
    // named PB-Bootstrap-1 and PB-Control-1, so the first protocol major is
    // 1 and no minor revision exists yet. Descriptor payloads use the separate
    // formal Protocol-1 Schema-1 envelope; historical provisional descriptor
    // vectors do not change the current protocol version.
    const ProtocolVersion protocolVersion{1, 0};
    return protocolVersion;
}

ProtocolResult<ProtocolVersionClassification> ClassifyProtocolVersion(
    const ProtocolVersion receivedVersion,
    const std::size_t fieldOffset)
{
    const ProtocolVersion currentVersion = GetProtocolVersion();
    return ClassifyProtocolVersionAgainst(
        receivedVersion,
        currentVersion,
        fieldOffset);
}

ProtocolResult<ProtocolVersionClassification> ClassifyProtocolVersionAgainst(
    const ProtocolVersion receivedVersion,
    const ProtocolVersion supportedVersion,
    const std::size_t fieldOffset)
{
    if (receivedVersion.major != supportedVersion.major)
    {
        return ProtocolResult<ProtocolVersionClassification>::Failure(
            ProtocolErrorCode::UnsupportedProtocolMajor,
            fieldOffset);
    }

    if (receivedVersion.minor > supportedVersion.minor)
    {
        return ProtocolResult<ProtocolVersionClassification>::Success(
            ProtocolVersionClassification::NewerMinorRequiresOptionalValidation);
    }

    return ProtocolResult<ProtocolVersionClassification>::Success(
        ProtocolVersionClassification::CurrentOrOlderMinor);
}

} // namespace pbprotocol
