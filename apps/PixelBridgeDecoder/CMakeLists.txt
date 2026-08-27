# Keep the no-argument shell deterministic. The native region selector does
# not introduce Qt or a capture/payload path.
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
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PixelBridgeDecoder PRIVATE PB::PBScreenRegion)
    target_compile_definitions(PixelBridgeDecoder PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(PixelBridgeDecoder PRIVATE /external:anglebrackets /external:W0)
    endif()
endif()
