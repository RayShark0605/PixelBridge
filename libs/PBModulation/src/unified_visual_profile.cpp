#include "pbmodulation/unified_visual_profile.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbprotocol/transport_block_codec.h"

namespace pbmodulation
{

static_assert(kUnifiedVisualProfile.innerFecProfileId == pbinnerfec::kInnerFecProfileIdRobust);
static_assert(kUnifiedVisualProfile.innerCodewordBits == pbinnerfec::kDvbS2ShortFrameNBits);
static_assert(kUnifiedVisualProfile.innerCodewordBits / 8 == pbinnerfec::kDvbS2ShortFrameCodewordByteCount);
static_assert(kUnifiedVisualProfile.transportOverheadBytes == pbprotocol::kTransportMinimumBlockBytes);

bool ValidateUnifiedVisualProfile() noexcept
{
    if (!ValidateUnifiedVisualProfileStaticContract())
    {
        return false;
    }
    const pbinnerfec::InnerFecProfile* const innerFecProfile =
        pbinnerfec::GetInnerFecProfile(kUnifiedVisualProfile.innerFecProfileId);
    return innerFecProfile != nullptr && innerFecProfile->profileId == pbinnerfec::kInnerFecProfileIdRobust &&
        innerFecProfile->nBits == kUnifiedVisualProfile.innerCodewordBits &&
        innerFecProfile->kBits == kUnifiedVisualProfile.innerInformationBits &&
        innerFecProfile->GetCodewordByteCount() == kUnifiedVisualProfile.innerCodewordBits / 8 &&
        innerFecProfile->GetInfoByteCount() == kUnifiedVisualProfile.innerInformationBits / 8 &&
        pbprotocol::kTransportMinimumBlockBytes == kUnifiedVisualProfile.transportOverheadBytes;
}

} // namespace pbmodulation
