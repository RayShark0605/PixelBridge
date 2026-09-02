PbAddUnitTest(PBDemodD3D11Tests test_demodulator.cpp)
target_link_libraries(PBDemodD3D11Tests PRIVATE PB::PBDemodD3D11 PB::PBModulation PB::PBProtocol
    PB::PBDesktopLevelsReference PB::PBInnerFec d3d11 dxgi)
target_compile_definitions(PBDemodD3D11Tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBDemodD3D11Tests)
set(pbDemodD3D11TestTimeout 180)
if(MSVC AND PB_BUILD_FUZZERS)
    # The instrumented full 17-case WARP matrix measures about five minutes on
    # the current host. Keep a bounded timeout without terminating a healthy
    # ASan run at the Release-oriented 180-second limit.
    set(pbDemodD3D11TestTimeout 420)
endif()
set_tests_properties(PBDemodD3D11Tests PROPERTIES
    LABELS "demod;d3d11;warp;shape-chroma;desktop-levels;fec"
    TIMEOUT ${pbDemodD3D11TestTimeout})
unset(pbDemodD3D11TestTimeout)

add_test(NAME PBRemoteVisualGpuParityTests
    COMMAND PBDemodD3D11Tests "[.gpu-parity]" --reporter console --durations yes)
set_tests_properties(PBRemoteVisualGpuParityTests PROPERTIES
    LABELS "demod;d3d11;remote-visual;gpu-parity;hardware"
    RUN_SERIAL TRUE
    TIMEOUT 300)
