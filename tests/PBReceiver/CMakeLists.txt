PbAddUnitTest(PBReceiverTests
    test_receiver_ingress.cpp
    test_resume_replay.cpp
    test_resume_state_file_io.cpp
    test_resume_restart_integration.cpp)

target_include_directories(PBReceiverTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBOuterFec/src")

target_compile_definitions(PBReceiverTests
    PRIVATE
        "PB_TEST_SCRATCH_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}\"")

target_link_libraries(PBReceiverTests
    PRIVATE
        PB::PBReceiver)
