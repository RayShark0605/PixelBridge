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
