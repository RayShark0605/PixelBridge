PbAddUnitTest(PBCaptureRotationTests test_rotation.cpp)
target_link_libraries(PBCaptureRotationTests PRIVATE PB::PBCaptureNormalize d3d11 dxgi)
target_include_directories(PBCaptureRotationTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_compile_definitions(PBCaptureRotationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBCaptureRotationTests)
set_tests_properties(PBCaptureRotationTests PROPERTIES LABELS "capture;normalize;capture-d3d" TIMEOUT 90)

PbAddUnitTest(PBCaptureNormalizeTests test_timestamp.cpp test_native_support.cpp test_runtime.cpp test_contract.cpp test_readback.cpp)
target_link_libraries(PBCaptureNormalizeTests PRIVATE PB::PBCaptureNormalize d3d11 ole32)
target_include_directories(PBCaptureNormalizeTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src"
    "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/src" "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/include")
target_compile_definitions(PBCaptureNormalizeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBCaptureNormalizeTests)
set_tests_properties(PBCaptureNormalizeTests PROPERTIES LABELS "capture;normalize;capture-unit" TIMEOUT 60)

PbAddUnitTest(PBCapturePipelineTests test_pipeline.cpp)
target_link_libraries(PBCapturePipelineTests PRIVATE PB::PBScreenCaptureWgc PB::PBScreenCaptureDxgi d3d11 dxgi ole32)
target_include_directories(PBCapturePipelineTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_compile_definitions(PBCapturePipelineTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBCapturePipelineTests)
set_tests_properties(PBCapturePipelineTests PROPERTIES LABELS "capture;normalize;capture-d3d;integration" TIMEOUT 60)

PbAddUnitTest(PBDecoderCliTests test_decoder_arguments.cpp
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/capture_bootstrap_telemetry.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/bootstrap_diagnostic.cpp")
target_link_libraries(PBDecoderCliTests PRIVATE PB::PBCaptureNormalize PB::PBModulation PB::PBDesktopLevelsReference PB::PBProtocol PB::PBTelemetry)
target_compile_definitions(PBDecoderCliTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBDecoderCliTests)
set_tests_properties(PBDecoderCliTests PROPERTIES LABELS "bootstrap;decoder;telemetry" TIMEOUT 60)

add_executable(PBDecoderTelemetryProbe telemetry_probe.cpp
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/capture_bootstrap_telemetry.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/bootstrap_diagnostic.cpp")
target_link_libraries(PBDecoderTelemetryProbe PRIVATE PB::CompilerSettings PB::PBCaptureNormalize PB::PBModulation PB::PBDesktopLevelsReference PB::PBProtocol PB::PBTelemetry)
target_compile_definitions(PBDecoderTelemetryProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBDecoderTelemetryProbe PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBDecoderTelemetryProbe)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBDecoderTelemetryProbe)
add_test(NAME PBDecoderTelemetryJson
    COMMAND "${CMAKE_COMMAND}" "-DPB_PROBE=$<TARGET_FILE:PBDecoderTelemetryProbe>" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTelemetryJson.cmake")
set_tests_properties(PBDecoderTelemetryJson PROPERTIES LABELS "bootstrap;decoder;telemetry" TIMEOUT 20)

PbAddUnitTest(PBBootstrapDiagnosticTests test_bootstrap_diagnostic.cpp test_desktop_levels_diagnostic.cpp
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/bootstrap_diagnostic.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/capture_bootstrap_telemetry.cpp")
target_link_libraries(PBBootstrapDiagnosticTests PRIVATE PB::PBCaptureNormalize PB::PBModulation PB::PBDesktopLevelsReference PB::PBProtocol PB::PBTelemetry d3d11 dxgi)
target_compile_definitions(PBBootstrapDiagnosticTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    PB_DESKTOP_LEVELS_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/desktop-levels")
PbInstrumentPresentationTest(PBBootstrapDiagnosticTests)
set_tests_properties(PBBootstrapDiagnosticTests PROPERTIES LABELS "bootstrap;capture-d3d;decoder;integration" TIMEOUT 180)

if(PB_BUILD_LOCAL_DESKTOP_GATE OR PB_BUILD_DESKTOP_LEVELS_GATE)
    PbAddUnitTest(PBLocalDesktopNativeTests test_bootstrap_native.cpp
        "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/bootstrap_diagnostic.cpp"
        "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/capture_bootstrap_cli.cpp"
        "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/capture_bootstrap_telemetry.cpp"
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PBLocalDesktopNativeTests PRIVATE PB::PBRenderD3D PB::PBScreenCaptureWgc PB::PBScreenCaptureDxgi PB::PBModulation PB::PBDesktopLevelsReference PB::PBProtocol PB::PBTelemetry d3d11 dxgi)
    target_include_directories(PBLocalDesktopNativeTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
    target_compile_definitions(PBLocalDesktopNativeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        PB_LOCAL_DESKTOP_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/local-desktop-bootstrap")
    PbInstrumentPresentationTest(PBLocalDesktopNativeTests)
    set_tests_properties(PBLocalDesktopNativeTests PROPERTIES LABELS "bootstrap;capture-native;local-desktop-native" TIMEOUT 120 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
    if(PB_BUILD_DESKTOP_LEVELS_GATE)
        target_compile_definitions(PBLocalDesktopNativeTests PRIVATE PB_DESKTOP_LEVELS_NATIVE_GATE=1)
        set_tests_properties(PBLocalDesktopNativeTests PROPERTIES LABELS "bootstrap;capture-native;local-desktop-native;desktop-levels-native" TIMEOUT 240)
    endif()
    if(TARGET PixelBridgeEncoder)
        add_test(NAME PBLocalDesktopEncoderEntry
            COMMAND "${CMAKE_COMMAND}" "-DPB_ENCODER=$<TARGET_FILE:PixelBridgeEncoder>"
                "-DPB_EVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/native-evidence"
                -P "${PROJECT_SOURCE_DIR}/tests/PresentationGate/VerifyEncoderLocalDesktopEntry.cmake")
        set_tests_properties(PBLocalDesktopEncoderEntry PROPERTIES LABELS "bootstrap;encoder;local-desktop-native" TIMEOUT 120 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
    endif()
endif()
