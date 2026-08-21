PbAddUnitTest(PBProtocolTests
    test_protocol_version.cpp)

target_link_libraries(PBProtocolTests
    PRIVATE
        PB::PBProtocol)