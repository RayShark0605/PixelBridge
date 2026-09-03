#include "descriptor_test_helpers.h"

#include "pbprotocol/product_visual_profile.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

TEST_CASE("Product visual profile catalog contains only Unified LC4", "[pbprotocol][product-profile]")
{
    STATIC_REQUIRE(pbprotocol::kProductVisualProfiles.size() == 1);
    STATIC_REQUIRE(pbprotocol::kUnifiedVisualProfileName == std::string_view{"PB-Unified-LC4-V1"});
    STATIC_REQUIRE(pbprotocol::kUnifiedVisualProfileId == 0x5042554E494C4331ULL);
    STATIC_REQUIRE(pbprotocol::kUnifiedVisualLayoutVersion == 8);
    STATIC_REQUIRE(pbprotocol::kProductVisualProfiles.front() == pbprotocol::kUnifiedProductVisualProfile);
    STATIC_REQUIRE(pbprotocol::IsProductSessionVisualProfileId(pbprotocol::kUnifiedVisualProfileId));
    STATIC_REQUIRE(pbprotocol::FindProductVisualProfile(pbprotocol::kUnifiedVisualProfileId) ==
        &pbprotocol::kProductVisualProfiles.front());
}

TEST_CASE("Product visual profile admission rejects unknown identity and layout", "[pbprotocol][product-profile][admission]")
{
    using pbprotocol::ProductVisualProfileAdmission;
    STATIC_REQUIRE(pbprotocol::ValidateProductVisualProfile(
        pbprotocol::kUnifiedVisualProfileId, pbprotocol::kUnifiedVisualLayoutVersion) == ProductVisualProfileAdmission::Accepted);
    STATIC_REQUIRE(pbprotocol::ValidateProductVisualProfile(pbprotocol::kUnifiedVisualProfileId, 7) ==
        ProductVisualProfileAdmission::UnsupportedLayout);
    STATIC_REQUIRE(pbprotocol::ValidateProductVisualProfile(0, pbprotocol::kUnifiedVisualLayoutVersion) ==
        ProductVisualProfileAdmission::UnsupportedProfile);
    STATIC_REQUIRE(pbprotocol::ValidateProductVisualProfile(0x504252564C463431ULL, 7) ==
        ProductVisualProfileAdmission::UnsupportedProfile);
    STATIC_REQUIRE_FALSE(pbprotocol::IsProductSessionVisualProfileId(0x504252564C463431ULL));
    STATIC_REQUIRE(pbprotocol::FindProductVisualProfile(0x504252564C463431ULL) == nullptr);
}

TEST_CASE("Generic descriptor validation preserves explicit internal historical profiles",
    "[pbprotocol][product-profile][internal-profile]")
{
    constexpr std::uint64_t historicalRemoteVisualLowFpsProfileId = 0x504252564C463431ULL;
    pbprotocol::SessionDescriptor descriptor = pbprotocol::test::MakeSessionDescriptor(0, 0);
    descriptor.sessionVisualProfileId = historicalRemoteVisualLowFpsProfileId;

    REQUIRE(pbprotocol::ValidateSessionDescriptor(descriptor, pbprotocol::test::MakeResourcePolicy()));
    REQUIRE_FALSE(pbprotocol::IsProductSessionVisualProfileId(descriptor.sessionVisualProfileId));
    REQUIRE(pbprotocol::ValidateProductVisualProfile(descriptor.sessionVisualProfileId, 7) ==
        pbprotocol::ProductVisualProfileAdmission::UnsupportedProfile);
}
