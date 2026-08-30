PbAddUnitTest(PBScreenCaptureWgcTests test_capture.cpp)
target_link_libraries(PBScreenCaptureWgcTests PRIVATE PB::PBScreenCaptureWgc)
target_include_directories(PBScreenCaptureWgcTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/src")
target_compile_definitions(PBScreenCaptureWgcTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBScreenCaptureWgcTests)
set_tests_properties(PBScreenCaptureWgcTests PROPERTIES LABELS "wgc;wgc-unit" TIMEOUT 60)

PbAddUnitTest(PBWgcD3dTests test_d3d_roi.cpp)
target_link_libraries(PBWgcD3dTests PRIVATE PB::PBScreenCaptureWgc d3d11 dxgi)
target_include_directories(PBWgcD3dTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/src")
target_compile_definitions(PBWgcD3dTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBWgcD3dTests)
set_tests_properties(PBWgcD3dTests PROPERTIES LABELS "wgc;wgc-d3d" TIMEOUT 60)

PbAddUnitTest(PBWgcCapabilitiesTests test_capabilities.cpp)
target_link_libraries(PBWgcCapabilitiesTests PRIVATE PB::PBScreenCaptureWgc)
target_include_directories(PBWgcCapabilitiesTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/src")
target_compile_definitions(PBWgcCapabilitiesTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBWgcCapabilitiesTests)
set_tests_properties(PBWgcCapabilitiesTests PROPERTIES LABELS "wgc;wgc-unit" TIMEOUT 30)

add_executable(PBWgcDpiProbe dpi_awareness_probe.cpp)
target_link_libraries(PBWgcDpiProbe PRIVATE PB::CompilerSettings PB::PBScreenCaptureWgc)
target_compile_definitions(PBWgcDpiProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBWgcDpiProbe PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBWgcDpiProbe)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBWgcDpiProbe)
add_test(NAME PBWgcRejectsNonPmv2Host COMMAND PBWgcDpiProbe)
set_tests_properties(PBWgcRejectsNonPmv2Host PROPERTIES LABELS "wgc;wgc-unit" TIMEOUT 15)

add_test(NAME PBWgcBuildOptions
    COMMAND "${CMAKE_COMMAND}" "-DPB_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
        "-DPB_PROBE_SCRIPT=${PROJECT_SOURCE_DIR}/tests/CMake/BuildOptionValidationProbe.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyBuildOptions.cmake")
set_tests_properties(PBWgcBuildOptions PROPERTIES LABELS "wgc;wgc-unit" TIMEOUT 30)

if(PB_BUILD_WGC_GATE)
    PbAddUnitTest(PBWgcNativeTests test_native_capture.cpp
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PBWgcNativeTests PRIVATE PB::PBScreenCaptureWgc PB::PBRenderD3D d3d11 dxgi)
    target_include_directories(PBWgcNativeTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureWgc/src" "${PROJECT_SOURCE_DIR}/libs/PBRenderD3D/src")
    target_compile_definitions(PBWgcNativeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        PB_TEST_MONITOR_ORIGIN_X=${PB_DESKTOP_TEST_MONITOR_ORIGIN_X}
        PB_TEST_MONITOR_ORIGIN_Y=${PB_DESKTOP_TEST_MONITOR_ORIGIN_Y})
    PbInstrumentPresentationTest(PBWgcNativeTests)
    set_tests_properties(PBWgcNativeTests PROPERTIES LABELS "wgc;wgc-native" TIMEOUT 120 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
endif()
