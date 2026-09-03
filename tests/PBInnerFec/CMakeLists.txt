PbAddUnitTest(PBInnerFecTests
    test_inner_fec_decoder.cpp
    test_inner_fec_errors_and_crc.cpp
    test_inner_fec_encoder_golden.cpp
    test_inner_fec_matrix_reference.cpp
    test_inner_fec_profile.cpp)

target_include_directories(PBInnerFecTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBInnerFec/src")

target_link_libraries(PBInnerFecTests
    PRIVATE
        PB::PBInnerFec
        PB::PBProtocol)

PbAddUnitTest(PBInnerFecUnifiedTests test_inner_fec_unified.cpp)
target_link_libraries(PBInnerFecUnifiedTests PRIVATE PB::PBInnerFec PB::PBProtocol)
set_tests_properties(PBInnerFecUnifiedTests PROPERTIES LABELS "inner-fec;unified" TIMEOUT 30)
