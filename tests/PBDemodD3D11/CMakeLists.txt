PbAddUnitTest(PBDemodD3D11Tests test_demodulator.cpp)
target_link_libraries(PBDemodD3D11Tests PRIVATE PB::PBDemodD3D11 PB::PBModulation PB::PBProtocol
    PB::PBDesktopLevelsReference d3d11 dxgi)
target_compile_definitions(PBDemodD3D11Tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBDemodD3D11Tests)
set_tests_properties(PBDemodD3D11Tests PROPERTIES LABELS "demod;d3d11;warp;shape-chroma;desktop-levels;fec" TIMEOUT 180)
