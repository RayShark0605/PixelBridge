# The expected version is the top-level project() version, so the test
# compares the library against CMake's single source of truth.
PbAddUnitTest(PBCoreTests
    test_build_info.cpp)

target_compile_definitions(PBCoreTests
    PRIVATE
        PB_CORE_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(PBCoreTests
    PRIVATE
        PB::PBCore)