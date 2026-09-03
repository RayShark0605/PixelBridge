#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace pbprotocol
{

struct ProductVisualProfile
{
    std::string_view name{};
    std::uint64_t visualProfileId = 0;
    std::uint8_t visualLayoutVersion = 0;

    bool operator==(const ProductVisualProfile&) const = default;
};

// Product admission is deliberately narrower than Protocol descriptor
// validation. PBProtocol continues to parse any non-zero profile identity so
// internal historical tools can explicitly exercise their frozen profiles;
// product application entry points must pass this catalog gate.
inline constexpr std::string_view kUnifiedVisualProfileName = "PB-Unified-LC4-V1";
inline constexpr std::uint64_t kUnifiedVisualProfileId = 0x5042554E494C4331ULL;
inline constexpr std::uint8_t kUnifiedVisualLayoutVersion = 8;
inline constexpr ProductVisualProfile kUnifiedProductVisualProfile{
    kUnifiedVisualProfileName, kUnifiedVisualProfileId, kUnifiedVisualLayoutVersion};
inline constexpr std::array<ProductVisualProfile, 1> kProductVisualProfiles{kUnifiedProductVisualProfile};

enum class ProductVisualProfileAdmission : std::uint8_t
{
    Accepted,
    UnsupportedProfile,
    UnsupportedLayout
};

[[nodiscard]] constexpr const ProductVisualProfile* FindProductVisualProfile(const std::uint64_t visualProfileId) noexcept
{
    for (const ProductVisualProfile& profile : kProductVisualProfiles)
    {
        if (profile.visualProfileId == visualProfileId)
        {
            return &profile;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr bool IsProductSessionVisualProfileId(const std::uint64_t visualProfileId) noexcept
{
    return FindProductVisualProfile(visualProfileId) != nullptr;
}

[[nodiscard]] constexpr ProductVisualProfileAdmission ValidateProductVisualProfile(
    const std::uint64_t visualProfileId, const std::uint8_t visualLayoutVersion) noexcept
{
    const ProductVisualProfile* const profile = FindProductVisualProfile(visualProfileId);
    if (profile == nullptr)
    {
        return ProductVisualProfileAdmission::UnsupportedProfile;
    }
    return profile->visualLayoutVersion == visualLayoutVersion ? ProductVisualProfileAdmission::Accepted :
        ProductVisualProfileAdmission::UnsupportedLayout;
}

static_assert(kProductVisualProfiles.size() == 1);
static_assert(kProductVisualProfiles.front() == kUnifiedProductVisualProfile);
static_assert(ValidateProductVisualProfile(kUnifiedVisualProfileId, kUnifiedVisualLayoutVersion) ==
    ProductVisualProfileAdmission::Accepted);

} // namespace pbprotocol
