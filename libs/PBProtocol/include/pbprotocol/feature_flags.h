#pragma once

#include "pbprotocol/protocol_result.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pbprotocol {

template <typename MaskType>
concept FeatureMask =
    std::same_as<std::remove_cv_t<MaskType>, std::uint16_t> ||
    std::same_as<std::remove_cv_t<MaskType>, std::uint32_t> ||
    std::same_as<std::remove_cv_t<MaskType>, std::uint64_t>;

template <FeatureMask MaskType>
struct FeatureFlags
{
    MaskType mandatory = 0;
    MaskType optional = 0;
};

template <FeatureMask MaskType>
struct FeatureFlagValidation
{
    MaskType knownMandatory = 0;
    MaskType knownOptional = 0;
    MaskType unknownOptional = 0;
};

template <FeatureMask MaskType>
[[nodiscard]] ProtocolResult<FeatureFlagValidation<MaskType>> ValidateFeatureFlags(
    const FeatureFlags<MaskType> featureFlags,
    const MaskType knownMask,
    const std::size_t fieldOffset = 0) noexcept
{
    const MaskType overlappingMask =
        static_cast<MaskType>(featureFlags.mandatory & featureFlags.optional);
    if (overlappingMask != 0)
    {
        return ProtocolResult<FeatureFlagValidation<MaskType>>::Failure(
            ProtocolErrorCode::ConflictingFeatureFlags,
            fieldOffset);
    }

    const MaskType unknownMandatory = static_cast<MaskType>(
        featureFlags.mandatory & static_cast<MaskType>(~knownMask));
    if (unknownMandatory != 0)
    {
        return ProtocolResult<FeatureFlagValidation<MaskType>>::Failure(
            ProtocolErrorCode::UnknownMandatoryFeature,
            fieldOffset);
    }

    const FeatureFlagValidation<MaskType> validation{
        static_cast<MaskType>(featureFlags.mandatory & knownMask),
        static_cast<MaskType>(featureFlags.optional & knownMask),
        static_cast<MaskType>(
            featureFlags.optional & static_cast<MaskType>(~knownMask))};
    return ProtocolResult<FeatureFlagValidation<MaskType>>::Success(validation);
}

} // namespace pbprotocol
