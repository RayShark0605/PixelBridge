# Keep the no-argument shell deterministic. Native diagnostic commands are
# explicit, never a hidden payload path or a full receiver UI.
add_executable(PixelBridgeDecoder
    main.cpp)

target_link_libraries(PixelBridgeDecoder
    PRIVATE
        PB::CompilerSettings
        PB::PBCore
        PB::PBProtocol
        PB::PBReceiver)

if(WIN32)
    target_sources(PixelBridgeDecoder PRIVATE screen_region_cli.cpp
        capture_bootstrap_cli.cpp bootstrap_diagnostic.cpp capture_bootstrap_telemetry.cpp
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PixelBridgeDecoder PRIVATE PB::PBScreenRegion PB::PBScreenCaptureWgc PB::PBScreenCaptureDxgi PB::PBModulation PB::PBDesktopLevelsReference)
    target_compile_definitions(PixelBridgeDecoder PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(PixelBridgeDecoder PRIVATE /external:anglebrackets /external:W0)
    endif()
endif()
