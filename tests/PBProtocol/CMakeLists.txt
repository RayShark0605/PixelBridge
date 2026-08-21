PbAddUnitTest(PBProtocolTests
    test_byte_io.cpp
    test_byte_io_uint8.cpp
    test_descriptor_binding.cpp
    test_descriptor_codec.cpp
    test_descriptor_validation.cpp
    test_feature_flags.cpp
    test_session_identifiers.cpp
    test_protocol_version.cpp)

target_link_libraries(PBProtocolTests
    PRIVATE
        PB::PBProtocol)
