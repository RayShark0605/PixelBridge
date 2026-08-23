PbAddUnitTest(PBCompressionTests
    test_segment_compression.cpp)

target_link_libraries(PBCompressionTests
    PRIVATE
        PB::PBCompression
        PB::PBProtocol)