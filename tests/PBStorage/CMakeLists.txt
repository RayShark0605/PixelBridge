PbAddUnitTest(PBStorageTests test_output_file.cpp)
target_link_libraries(PBStorageTests PRIVATE PB::PBStorage PB::PBProtocol)
target_compile_definitions(PBStorageTests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
PbInstrumentPresentationTest(PBStorageTests)
set_tests_properties(PBStorageTests PROPERTIES LABELS "storage;publish;digest" TIMEOUT 30)
