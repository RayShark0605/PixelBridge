find_package(wirehair 2.0 CONFIG REQUIRED)

PbAddUnitTest(PBOuterFecTests
    test_direct_repeat.cpp
    test_orphan_admission_flow.cpp
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

PbAddUnitTest(PBOuterFecLifetimeTests
    test_decoder_reservation_lifetime.cpp)
target_include_directories(PBOuterFecLifetimeTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBOuterFec/src")
target_link_libraries(PBOuterFecLifetimeTests
    PRIVATE
        PB::PBOuterFec)
set_tests_properties(PBOuterFecLifetimeTests PROPERTIES
    LABELS "pbouterfec;resource;lifetime"
    TIMEOUT 10)
