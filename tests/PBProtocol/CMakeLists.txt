PbAddUnitTest(PBProtocolTests
    test_byte_io.cpp
    test_feature_flags.cpp
    test_protocol_version.cpp)

target_link_libraries(PBProtocolTests
    PRIVATE
        PB::PBProtocol)
