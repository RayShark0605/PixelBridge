# The no-argument console shell remains deterministic. The explicit data
# window path is independent of the future Qt control UI.
add_executable(PixelBridgeEncoder
    main.cpp)

target_link_libraries(PixelBridgeEncoder
    PRIVATE
        PB::CompilerSettings
        PB::PBCore
        PB::PBProtocol)

if(WIN32)
    target_sources(PixelBridgeEncoder PRIVATE presentation_cli.cpp
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PixelBridgeEncoder PRIVATE PB::PBRenderD3D PB::PBModulation PB::PBDesktopLevelsReference)
    target_compile_definitions(PixelBridgeEncoder PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(PixelBridgeEncoder PRIVATE /external:anglebrackets /external:W0)
    endif()
endif()
