PbAddUnitTest(PBDemodD3D11Tests test_demodulator.cpp test_g12_semantic_parity.cpp)
target_link_libraries(PBDemodD3D11Tests PRIVATE PB::PBDemodD3D11 PB::PBModulation PB::PBProtocol
    PB::PBDesktopLevelsReference PB::PBInnerFec PB::PBOuterFec PB::PBReceiver PB::PBRemoteVisualSimulator
    PB::PBStorage bcrypt d3d11 dxgi)
target_compile_definitions(PBDemodD3D11Tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_G12_PARITY_REPORT=\"${CMAKE_CURRENT_BINARY_DIR}/g12-unified-gpu-parity.actual.jsonl\""
    "PB_G12_PERFORMANCE_REPORT=\"${CMAKE_CURRENT_BINARY_DIR}/g12-unified-publish-metrics.actual.jsonl\""
    "PB_G12_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
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

add_test(NAME PBUnifiedVisualHardwareSmokeTests
    COMMAND PBDemodD3D11Tests "[.unified-hardware-smoke]" --reporter console --durations yes)
set_tests_properties(PBUnifiedVisualHardwareSmokeTests PROPERTIES
    LABELS "demod;d3d11;unified;hardware-smoke"
    RUN_SERIAL TRUE
    TIMEOUT 300)

add_test(NAME PBUnifiedVisualGpuParityTests
    COMMAND PBDemodD3D11Tests "[.unified-g12]" --reporter console --durations yes --rng-seed 12122026)
set_tests_properties(PBUnifiedVisualGpuParityTests PROPERTIES
    LABELS "demod;d3d11;unified;gpu-parity;hardware;receiver;storage;performance"
    RUN_SERIAL TRUE
    TIMEOUT 1800)
