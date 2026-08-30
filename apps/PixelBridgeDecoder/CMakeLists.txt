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
        decoder_runtime_cli.cpp
        decoder_application_controller.cpp decoder_application_controller.h decoder_gui.cpp decoder_gui.h
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PixelBridgeDecoder PRIVATE PB::PBScreenRegion PB::PBScreenCaptureWgc PB::PBScreenCaptureDxgi PB::PBModulation PB::PBDesktopLevelsReference PB::PBTelemetry
        PB::PBApplication Qt${QT_VERSION_MAJOR}::Widgets)
    execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE pbDecoderGitCommit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT pbDecoderGitCommit)
        set(pbDecoderGitCommit "unknown")
    endif()
    target_compile_definitions(PixelBridgeDecoder PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        PB_ENABLE_QT_GUI=1 PB_ENABLE_APPLICATION_RUNTIME=1 "PB_GIT_COMMIT=\"${pbDecoderGitCommit}\"")
    if(MSVC)
        target_compile_options(PixelBridgeDecoder PRIVATE /external:anglebrackets /external:W0)
    endif()
    get_target_property(pbDecoderQtQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    if(pbDecoderQtQmake)
        get_filename_component(pbDecoderQtBin "${pbDecoderQtQmake}" DIRECTORY)
        find_program(pbDecoderWindeployqt NAMES windeployqt HINTS "${pbDecoderQtBin}" NO_DEFAULT_PATH)
        if(pbDecoderWindeployqt)
            add_custom_command(TARGET PixelBridgeDecoder POST_BUILD COMMAND "${pbDecoderWindeployqt}"
                --no-translations --no-opengl-sw "$<TARGET_FILE:PixelBridgeDecoder>" VERBATIM)
        endif()
    endif()
    unset(pbDecoderGitCommit)
    unset(pbDecoderQtQmake)
    unset(pbDecoderQtBin)
    unset(pbDecoderWindeployqt)
endif()
