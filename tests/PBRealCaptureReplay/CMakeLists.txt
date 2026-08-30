PbAddUnitTest(PBRealCaptureReplayTests test_replay.cpp test_replay_v2.cpp)
target_link_libraries(PBRealCaptureReplayTests PRIVATE PB::PBRealCaptureReplay PB::PBDemodD3D11
    PB::PBModulation PB::PBDesktopLevelsReference PB::PBProtocol d3d11 dxgi)
target_compile_definitions(PBRealCaptureReplayTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBRealCaptureReplayTests)
set_tests_properties(PBRealCaptureReplayTests PROPERTIES LABELS "replay;capture;demod;d3d11;warp;fec;file-safety" TIMEOUT 240)
