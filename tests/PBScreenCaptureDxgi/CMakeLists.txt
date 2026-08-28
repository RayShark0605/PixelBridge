PbAddUnitTest(PBScreenCaptureDxgiTests test_policy.cpp test_duplication.cpp test_runtime.cpp)
target_link_libraries(PBScreenCaptureDxgiTests PRIVATE PB::PBScreenCaptureDxgi dxgi)
target_include_directories(PBScreenCaptureDxgiTests PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureDxgi/src" "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_compile_definitions(PBScreenCaptureDxgiTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBScreenCaptureDxgiTests)
set_tests_properties(PBScreenCaptureDxgiTests PROPERTIES LABELS "dxgi;dxgi-unit" TIMEOUT 60)

if(PB_BUILD_DXGI_GATE)
    PbAddUnitTest(PBDxgiNativeTests test_native_capture.cpp test_environment.cpp "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PBDxgiNativeTests PRIVATE PB::PBScreenCaptureDxgi PB::PBRenderD3D d3d11 dxgi)
    target_include_directories(PBDxgiNativeTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenCaptureDxgi/src"
        "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src" "${PROJECT_SOURCE_DIR}/libs/PBRenderD3D/src")
    target_compile_definitions(PBDxgiNativeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    PbInstrumentPresentationTest(PBDxgiNativeTests)
    set_tests_properties(PBDxgiNativeTests PROPERTIES LABELS "dxgi;dxgi-native" TIMEOUT 120 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
endif()
