PbAddUnitTest(PBProtocolTests
    test_byte_io.cpp
    test_byte_io_uint8.cpp
    test_descriptor_binding.cpp
    test_descriptor_codec.cpp
    test_descriptor_validation.cpp
    test_feature_flags.cpp
    test_protocol_result.cpp
    test_session_registry.cpp
    test_session_identifiers.cpp
    test_protocol_version.cpp
    test_blake3_digest.cpp
    test_checked_integer.cpp
    test_crc32c.cpp
    test_descriptor_corpus.cpp
    test_session_random.cpp)

target_link_libraries(PBProtocolTests
    PRIVATE
        PB::PBProtocol)

target_include_directories(PBProtocolTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBProtocol/src")

file(TO_CMAKE_PATH
    "${PROJECT_SOURCE_DIR}/fuzz/corpus/descriptor-resource"
    pbDescriptorCorpusDirectory)
target_compile_definitions(PBProtocolTests
    PRIVATE
        "PB_DESCRIPTOR_CORPUS_DIRECTORY=\"${pbDescriptorCorpusDirectory}\"")
unset(pbDescriptorCorpusDirectory)

add_executable(PBProtocolNoAllocationDigestProbe
    no_allocation_digest_probe.cpp)
target_link_libraries(PBProtocolNoAllocationDigestProbe
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)
add_test(
    NAME PBProtocolNoAllocationDigestProbe
    COMMAND PBProtocolNoAllocationDigestProbe)
set_tests_properties(PBProtocolNoAllocationDigestProbe PROPERTIES
    LABELS "pbprotocol;blake3;allocation"
    TIMEOUT 10)
