find_program(PB_DESKTOP_LEVELS_POWERSHELL NAMES pwsh REQUIRED)
add_test(NAME PBDesktopLevelsStaticReviewValidator COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStaticReview.ps1" -ScratchRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/static-review")
set_tests_properties(PBDesktopLevelsStaticReviewValidator PROPERTIES LABELS "desktop-levels;gate-validation" TIMEOUT 30)

add_test(NAME PBDesktopLevelsGateJsonTests COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyGateJson.ps1" -ScratchRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/gate-json")
set_tests_properties(PBDesktopLevelsGateJsonTests PROPERTIES LABELS "desktop-levels;gate-validation" TIMEOUT 30)

add_test(NAME PBDesktopLevelsNativeLoopPolicyTests COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNativeLoopPolicy.ps1")
set_tests_properties(PBDesktopLevelsNativeLoopPolicyTests PROPERTIES LABELS "desktop-levels;gate-validation" TIMEOUT 30)

add_test(NAME PBPhase1FileGatePolicyTests COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPhase1FileGatePolicy.ps1")
set_tests_properties(PBPhase1FileGatePolicyTests PROPERTIES LABELS "phase1;desktop-levels;file-gate;gate-validation" TIMEOUT 30)

add_test(NAME PBPhase1FileResourcesTests COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPhase1FileResources.ps1")
set_tests_properties(PBPhase1FileResourcesTests PROPERTIES LABELS "phase1;desktop-levels;file-gate;soak;gate-validation" TIMEOUT 30)

PbAddUnitTest(PBPhase1FileGateArgumentsTests test_phase1_file_gate_arguments.cpp test_phase1_file_metrics.cpp)
target_include_directories(PBPhase1FileGateArgumentsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
PbInstrumentPresentationTest(PBPhase1FileGateArgumentsTests)
set_tests_properties(PBPhase1FileGateArgumentsTests PROPERTIES LABELS "phase1;desktop-levels;file-gate;arguments" TIMEOUT 30)

add_executable(PBPhase1FileGate phase1_file_gate.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBPhase1FileGate PRIVATE PB::CompilerSettings PB::PBProtocol PB::PBCompression PB::PBOuterFec
    PB::PBInnerFec PB::PBReceiver PB::PBRenderD3D PB::PBScreenRegion PB::PBScreenCaptureWgc PB::PBScreenCaptureDxgi
    PB::PBCaptureNormalize PB::PBDemodD3D11 PB::PBModulation PB::PBDesktopLevelsReference)
target_include_directories(PBPhase1FileGate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_compile_definitions(PBPhase1FileGate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBPhase1FileGate PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBPhase1FileGate)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBPhase1FileGate)

if(PB_BUILD_FUZZERS)
    set(phase1FileGateNativeMode asan)
else()
    set(phase1FileGateNativeMode release)
endif()
foreach(backend IN ITEMS wgc dxgi)
    foreach(profile IN ITEMS desktop-levels-2x2 shape-chroma)
        string(REPLACE "-" "_" profileName "${profile}")
        set(fileGateName "PBPhase1FileNative_${backend}_${profileName}")
        add_test(NAME ${fileGateName} COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/InvokePhase1FileLoop.ps1"
            -GateExecutable "$<TARGET_FILE:PBPhase1FileGate>" -Support "$<TARGET_FILE:PBDesktopLevelsNativeSupport>"
            -Backend "${backend}" -Profile "${profile}" -EvidenceRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/phase1-file-evidence"
            -NativeMode "${phase1FileGateNativeMode}"
            -MonitorOriginX "${PB_DESKTOP_TEST_MONITOR_ORIGIN_X}" -MonitorOriginY "${PB_DESKTOP_TEST_MONITOR_ORIGIN_Y}")
        set_tests_properties(${fileGateName} PROPERTIES
            LABELS "phase1;physical-layer;file-gate;whole-file-digest;desktop-levels-native;capture-native"
            TIMEOUT 140 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
    endforeach()
endforeach()

add_executable(PBDesktopLevelsNativeSupport native_support.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBDesktopLevelsNativeSupport PRIVATE PB::CompilerSettings PB::PBCaptureNormalize dwmapi)
target_include_directories(PBDesktopLevelsNativeSupport PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_compile_definitions(PBDesktopLevelsNativeSupport PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBDesktopLevelsNativeSupport PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBDesktopLevelsNativeSupport)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBDesktopLevelsNativeSupport)

foreach(backend IN ITEMS wgc dxgi)
    foreach(candidate IN ITEMS desktop-levels-2x2 desktop-levels-4x4 shape-chroma)
        string(REPLACE "-" "_" candidateName "${candidate}")
        set(gateName "PBPhysicalLayerNative_${backend}_${candidateName}")
        add_test(NAME ${gateName} COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/InvokeNativeLoop.ps1"
            -Encoder "$<TARGET_FILE:PixelBridgeEncoder>" -Decoder "$<TARGET_FILE:PixelBridgeDecoder>"
            -Support "$<TARGET_FILE:PBDesktopLevelsNativeSupport>" -Backend "${backend}" -Candidate "${candidate}"
            -EvidenceRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/evidence"
            -MonitorOriginX "${PB_DESKTOP_TEST_MONITOR_ORIGIN_X}" -MonitorOriginY "${PB_DESKTOP_TEST_MONITOR_ORIGIN_Y}")
        set_tests_properties(${gateName} PROPERTIES LABELS "physical-layer;desktop-levels;shape-chroma;desktop-levels-native;capture-native"
            TIMEOUT 100 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
    endforeach()
endforeach()
