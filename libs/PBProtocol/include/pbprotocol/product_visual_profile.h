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
inline constexpr std::string_view kUnifiedVisualProfileName = "PB-Unified-SC6-V3";
inline constexpr std::uint64_t kUnifiedVisualProfileId = 0x5042554E49534333ULL;
inline constexpr std::uint8_t kUnifiedVisualLayoutVersion = 10;
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

// Experimental blank-control identity (layout 11): the unified main-region
// contract plus two supplemental control bands in the blank canvas areas.
// Opt-in only — it is deliberately NOT part of kProductVisualProfiles, so
// product admission keeps rejecting it and it can never become the default
// wire identity of a released session. The PBProtocol layer continues to
// parse any non-zero profile id opaquely; applications bind this constant
// explicitly when the operator opts in.
inline constexpr ProductVisualProfile kBlankControlExperimentalProfile{
    "PB-Experimental-BlankControl-1",
    0x504242414E443031ULL,
    11};
static_assert(!IsProductSessionVisualProfileId(kBlankControlExperimentalProfile.visualProfileId));

// Experimental gray-state identity (layout 12): the unified layout-10
// manifest, mapping and slot contract unchanged; the four data-tile
// foreground states and the calibration state stripes switch from iso-luma
// chroma colors to four gray luma levels, so the second carrier's two bits
// survive chroma-destroying (4:2:0) remote links. Opt-in only — like the
// blank-control identity it stays outside kProductVisualProfiles so product
// admission rejects it and it can never become a released default.
inline constexpr ProductVisualProfile kGrayStatesExperimentalProfile{
    "PB-Experimental-GrayStates-1",
    0x5042475953544131ULL,
    12};
static_assert(!IsProductSessionVisualProfileId(kGrayStatesExperimentalProfile.visualProfileId));
static_assert(kGrayStatesExperimentalProfile.visualProfileId != kBlankControlExperimentalProfile.visualProfileId);
static_assert(kGrayStatesExperimentalProfile.visualProfileId != kUnifiedVisualProfileId);

} // namespace pbprotocol
