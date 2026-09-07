#include "hardware_adapter_selection.h"
#include "pbrenderd3d/data_window.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("Independent hardware selection is explicit complete and never software", "[presentation][adapter-compat]")
{
    using pbrenderd3d::HardwareAdapterCandidate;
    using pbrenderd3d::SelectHardwarePresentationAdapter;
    const std::array citrix{HardwareAdapterCandidate{false, false}, HardwareAdapterCandidate{true, false}};
    CHECK_FALSE(SelectHardwarePresentationAdapter(citrix, false, true));
    CHECK_FALSE(SelectHardwarePresentationAdapter(citrix, true, false));
    const auto selected = SelectHardwarePresentationAdapter(citrix, true, true);
    REQUIRE(selected);
    CHECK(selected->index == 0);
    CHECK_FALSE(selected->boundToMonitorOutput);
    const std::array softwareOnly{HardwareAdapterCandidate{true, false}};
    CHECK_FALSE(SelectHardwarePresentationAdapter(softwareOnly, true, true));
    CHECK_FALSE(SelectHardwarePresentationAdapter({}, true, true));
    const std::array softwareFirst{HardwareAdapterCandidate{true, false}, HardwareAdapterCandidate{false, false}};
    REQUIRE(SelectHardwarePresentationAdapter(softwareFirst, true, true));
    CHECK(SelectHardwarePresentationAdapter(softwareFirst, true, true)->index == 1);
}

TEST_CASE("A concrete monitor mapping takes precedence and cannot silently switch away from software", "[presentation][adapter-compat]")
{
    using pbrenderd3d::HardwareAdapterCandidate;
    using pbrenderd3d::SelectHardwarePresentationAdapter;
    const std::array mapped{HardwareAdapterCandidate{false, false}, HardwareAdapterCandidate{false, true}};
    for (const bool allowUnmapped : {false, true})
    {
        const auto selected = SelectHardwarePresentationAdapter(mapped, allowUnmapped, false);
        REQUIRE(selected);
        CHECK(selected->index == 1);
        CHECK(selected->boundToMonitorOutput);
    }
    const std::array mappedSoftware{HardwareAdapterCandidate{false, false}, HardwareAdapterCandidate{true, true}};
    CHECK_FALSE(SelectHardwarePresentationAdapter(mappedSoftware, true, true));
}

TEST_CASE("Hardware compatibility policy requires an explicitly located fullscreen sender", "[presentation][adapter-compat]")
{
    pbrenderd3d::DataWindowConfig config;
    CHECK_FALSE(config.allowUnmappedHardwareAdapter);
    config.allowUnmappedHardwareAdapter = true;
    CHECK_FALSE(pbrenderd3d::ValidateDataWindowConfig(config));
    config.topmost = true;
    CHECK_FALSE(pbrenderd3d::ValidateDataWindowConfig(config));
    config.clientOrigin = pbrenderd3d::PhysicalPoint{1920, -1};
    CHECK(pbrenderd3d::ValidateDataWindowConfig(config));
}
