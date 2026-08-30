PbAddUnitTest(PBRemoteVisualSimulatorTests test_channel_transform.cpp)
target_link_libraries(PBRemoteVisualSimulatorTests
    PRIVATE
        PB::PBRemoteVisualSimulator
        PB::PBModulation
        PB::PBProtocol
        PB::PBDesktopLevelsReference
        PB::PBInnerFec)
PbInstrumentPresentationTest(PBRemoteVisualSimulatorTests)
set_tests_properties(PBRemoteVisualSimulatorTests PROPERTIES
    LABELS "remote-visual;simulator;channel;resource"
    TIMEOUT 240)
