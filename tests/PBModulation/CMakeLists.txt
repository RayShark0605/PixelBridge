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
    target_link_libraries(PBLocalDesktopNoAllocationProbe PRIVATE PB::CompilerSettings PB::PBModulation PB::PBProtocol PB::PBDesktopLevelsReference)
    PbInstrumentPresentationTest(PBLocalDesktopNoAllocationProbe)
    set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBLocalDesktopNoAllocationProbe)
    add_test(NAME PBLocalDesktopNoAllocationProbe COMMAND PBLocalDesktopNoAllocationProbe)
    set_tests_properties(PBLocalDesktopNoAllocationProbe PROPERTIES LABELS "bootstrap;local-desktop;desktop-levels;allocation" TIMEOUT 30
        PASS_REGULAR_EXPRESSION "LOCAL_DESKTOP_NO_ALLOCATION_PASS")
endif()

PbAddUnitTest(PBDesktopLevelsTests test_desktop_levels.cpp test_desktop_levels_reference.cpp test_desktop_levels_channel.cpp)
target_link_libraries(PBDesktopLevelsTests PRIVATE PB::PBModulation PB::PBProtocol PB::PBInterleave PB::PBDesktopLevelsReference PB::PBInnerFec)
target_compile_definitions(PBDesktopLevelsTests PRIVATE PB_DESKTOP_LEVELS_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/desktop-levels")
PbInstrumentPresentationTest(PBDesktopLevelsTests)
set_tests_properties(PBDesktopLevelsTests PROPERTIES LABELS "desktop-levels;golden;modulation" TIMEOUT 180)

PbAddUnitTest(PBShapeChromaTests test_shape_chroma.cpp)
target_link_libraries(PBShapeChromaTests PRIVATE PB::PBModulation PB::PBProtocol PB::PBInterleave PB::PBDesktopLevelsReference PB::PBInnerFec)
PbInstrumentPresentationTest(PBShapeChromaTests)
set_tests_properties(PBShapeChromaTests PROPERTIES LABELS "shape-chroma;modulation;fec;integration" TIMEOUT 240)

PbAddUnitTest(PBRemoteVisualTests test_remote_visual.cpp test_remote_visual_low_fps.cpp test_remote_visual_low_fps_golden.cpp)
target_link_libraries(PBRemoteVisualTests PRIVATE PB::PBModulation PB::PBProtocol PB::PBInterleave PB::PBDesktopLevelsReference PB::PBInnerFec)
target_compile_definitions(PBRemoteVisualTests PRIVATE
    PB_REMOTE_VISUAL_LF4_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/remote-visual/lf4")
PbInstrumentPresentationTest(PBRemoteVisualTests)
set_tests_properties(PBRemoteVisualTests PROPERTIES LABELS "remote-visual;modulation;fec;golden" TIMEOUT 240)

PbAddUnitTest(PBUnifiedVisualProfileTests test_unified_visual_profile.cpp)
target_link_libraries(PBUnifiedVisualProfileTests PRIVATE PB::PBModulation PB::PBProtocol PB::PBInnerFec)
set_tests_properties(PBUnifiedVisualProfileTests PROPERTIES LABELS "modulation;product-profile;unified" TIMEOUT 20)

if(TARGET PBDesktopLevelsBaseline)
    add_test(NAME PBDesktopLevelsCpuBaseline COMMAND PBDesktopLevelsBaseline --baseline)
    set_tests_properties(PBDesktopLevelsCpuBaseline PROPERTIES LABELS "desktop-levels;integration;baseline" TIMEOUT 120)
    add_test(NAME PBDesktopLevelsBaselineJson COMMAND "${CMAKE_COMMAND}" "-DPB_BASELINE=$<TARGET_FILE:PBDesktopLevelsBaseline>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDesktopLevelsBaseline.cmake")
    set_tests_properties(PBDesktopLevelsBaselineJson PROPERTIES LABELS "desktop-levels;integration;telemetry" TIMEOUT 130)
    set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBDesktopLevelsBaseline)
endif()
