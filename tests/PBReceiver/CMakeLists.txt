PbAddUnitTest(PBReceiverTests
    test_receiver_ingress.cpp)

target_link_libraries(PBReceiverTests
    PRIVATE
        PB::PBReceiver)
