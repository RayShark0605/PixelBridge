find_package(wirehair 2.0 CONFIG REQUIRED)

PbAddUnitTest(PBOuterFecTests
    test_direct_repeat.cpp
    test_wirehair_v2.cpp
    test_wirehair_v2_errors.cpp)

target_include_directories(PBOuterFecTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBOuterFec/src")

target_link_libraries(PBOuterFecTests
    PRIVATE
        PB::PBCompression
        PB::PBOuterFec
        wirehair::wirehair)
