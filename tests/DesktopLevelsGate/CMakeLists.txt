find_program(PB_DESKTOP_LEVELS_POWERSHELL NAMES pwsh REQUIRED)
add_test(NAME PBDesktopLevelsStaticReviewValidator COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStaticReview.ps1" -ScratchRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/static-review")
set_tests_properties(PBDesktopLevelsStaticReviewValidator PROPERTIES LABELS "desktop-levels;gate-validation" TIMEOUT 30)

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
    foreach(tile IN ITEMS 2 4)
        set(gateName "PBDesktopLevelsNative_${backend}_${tile}x${tile}")
        add_test(NAME ${gateName} COMMAND "${PB_DESKTOP_LEVELS_POWERSHELL}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/InvokeNativeLoop.ps1"
            -Encoder "$<TARGET_FILE:PixelBridgeEncoder>" -Decoder "$<TARGET_FILE:PixelBridgeDecoder>"
            -Support "$<TARGET_FILE:PBDesktopLevelsNativeSupport>" -Backend "${backend}" -Tile "${tile}"
            -EvidenceRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/evidence")
        set_tests_properties(${gateName} PROPERTIES LABELS "desktop-levels;desktop-levels-native;capture-native"
            TIMEOUT 100 RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop)
    endforeach()
endforeach()
