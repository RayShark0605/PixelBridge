# PBModulation Phase-0 reference raster/demod test suite (Catch2 v3).
# The PNG version pin test needs <png.h>; find the pinned package in this
# directory scope (the result is cached tree-wide).
find_package(PNG 1.6.58 CONFIG REQUIRED)

PbAddUnitTest(PBModulationTests
    test_reference_visual_profile.cpp
    test_reference_raster.cpp
    test_reference_frame_io_raw.cpp
    test_reference_frame_io_png.cpp
    test_reference_golden.cpp
    test_reference_integration.cpp)

target_link_libraries(PBModulationTests
    PRIVATE
        PB::PBModulation
        PB::PBProtocol
        PB::PBInnerFec
        PNG::PNG)

# MSVC ASan runtime copy for this executable (fuzz-enabled trees instrument
# the linked core libraries) is handled centrally by PbAddUnitTest in
# tests/CMakeLists.txt.

# Independent experimental binding; the frozen ReferenceRaster suite and pins
# above remain unchanged and continue to run as their own compatibility gate.
PbAddUnitTest(PBLocalDesktopBootstrapTests test_bootstrap_rs.cpp test_local_desktop_bootstrap.cpp test_local_desktop_decode.cpp test_local_desktop_luma_calibration.cpp test_local_desktop_core_samples.cpp test_local_desktop_partial_tear.cpp)
target_link_libraries(PBLocalDesktopBootstrapTests PRIVATE PB::PBModulation PB::PBProtocol)
target_compile_definitions(PBLocalDesktopBootstrapTests PRIVATE
    PB_LOCAL_DESKTOP_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/local-desktop-bootstrap")
PbInstrumentPresentationTest(PBLocalDesktopBootstrapTests)
set_tests_properties(PBLocalDesktopBootstrapTests PROPERTIES LABELS "bootstrap;local-desktop;golden" TIMEOUT 120)

PbAddUnitTest(PBLocalDesktopMatrixTests test_local_desktop_matrix.cpp)
target_link_libraries(PBLocalDesktopMatrixTests PRIVATE PB::PBModulation PB::PBProtocol)
target_compile_definitions(PBLocalDesktopMatrixTests PRIVATE
    PB_LOCAL_DESKTOP_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/local-desktop-bootstrap")
PbInstrumentPresentationTest(PBLocalDesktopMatrixTests)
set_tests_properties(PBLocalDesktopMatrixTests PROPERTIES LABELS "bootstrap;local-desktop;geometry;mixed-frame" TIMEOUT 120)

if(WIN32)
    # A standalone process replaces C++ allocation functions without intercepting
    # Catch2/framework allocations or weakening the ordinary test executable.
    add_executable(PBLocalDesktopNoAllocationProbe local_desktop_no_allocation_probe.cpp)
    target_link_libraries(PBLocalDesktopNoAllocationProbe PRIVATE PB::CompilerSettings PB::PBModulation PB::PBProtocol)
    PbInstrumentPresentationTest(PBLocalDesktopNoAllocationProbe)
    set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBLocalDesktopNoAllocationProbe)
    add_test(NAME PBLocalDesktopNoAllocationProbe COMMAND PBLocalDesktopNoAllocationProbe)
    set_tests_properties(PBLocalDesktopNoAllocationProbe PROPERTIES LABELS "bootstrap;local-desktop;allocation" TIMEOUT 30
        PASS_REGULAR_EXPRESSION "LOCAL_DESKTOP_NO_ALLOCATION_PASS")
endif()
