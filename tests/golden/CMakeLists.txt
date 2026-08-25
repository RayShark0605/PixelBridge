# Golden vector verification harness (PBGoldenVectorCheck), its shared core,
# and the negative-sample Catch2 suite.
#
# The committed golden files (tests/golden/<category>/<name>.bin, all <=
# 66 KB) are checked in three gates; the frame 8.3 MB payloads are
# digest-pinned, not committed (docs/GOLDEN_VECTOR_HARNESS.md).

add_library(PBGoldenVectorCheckCore STATIC
    golden_vector_check_core.cpp)
target_link_libraries(PBGoldenVectorCheckCore
    PUBLIC
        PB::PBGoldenVector
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)
target_include_directories(PBGoldenVectorCheckCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(PBGoldenVectorCheck
    golden_vector_check_main.cpp)
target_link_libraries(PBGoldenVectorCheck
    PRIVATE
        PB::CompilerSettings
        PBGoldenVectorCheckCore)

add_test(
    NAME PBGoldenVectorCheck
    COMMAND PBGoldenVectorCheck
        "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(PBGoldenVectorCheck PROPERTIES
    LABELS "golden;harness"
    TIMEOUT 600)

PbAddUnitTest(PBGoldenVectorCheckTests
    test_golden_vector_check.cpp)
target_link_libraries(PBGoldenVectorCheckTests
    PRIVATE
        PBGoldenVectorCheckCore)
target_compile_definitions(PBGoldenVectorCheckTests
    PRIVATE
        "PB_GOLDEN_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "PB_TEST_SCRATCH_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}\"")
