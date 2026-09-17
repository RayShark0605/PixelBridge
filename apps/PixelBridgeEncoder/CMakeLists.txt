add_executable(PixelBridgeEncoder
    main.cpp)

target_link_libraries(PixelBridgeEncoder
    PRIVATE
        PB::CompilerSettings
        PB::PBCore
        PB::PBProtocol)

if(WIN32)
    configure_file(resources/application.rc.in "${CMAKE_CURRENT_BINARY_DIR}/application.rc" @ONLY)
    set_target_properties(PixelBridgeEncoder PROPERTIES AUTORCC ON)
    target_sources(PixelBridgeEncoder PRIVATE resources/branding.qrc "${CMAKE_CURRENT_BINARY_DIR}/application.rc"
        "${PROJECT_SOURCE_DIR}/apps/common/operational_log_qt.cpp")
    target_sources(PixelBridgeEncoder PRIVATE presentation_cli.cpp encoder_runtime_cli.cpp encoder_application_controller.cpp
        "${PROJECT_SOURCE_DIR}/apps/common/step1_gui_evidence_qt.cpp"
        encoder_application_controller.h encoder_gui.cpp encoder_gui.h
        "${PROJECT_SOURCE_DIR}/apps/common/remote_visual_metadata_preset_qt.cpp"
        "${PROJECT_SOURCE_DIR}/apps/common/remote_visual_metadata_preset_qt.h"
        "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
    target_link_libraries(PixelBridgeEncoder PRIVATE PB::PBRenderD3D PB::PBModulation PB::PBDesktopLevelsReference
        PB::PBApplication Qt${QT_VERSION_MAJOR}::Widgets)
    execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE pbEncoderGitCommit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT pbEncoderGitCommit)
        set(pbEncoderGitCommit "unknown")
    endif()
    target_compile_definitions(PixelBridgeEncoder PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        PB_ENABLE_QT_GUI=1 PB_ENABLE_APPLICATION_RUNTIME=1 "PB_GIT_COMMIT=\"${pbEncoderGitCommit}\"")
    if(MSVC)
        # Keep the normal Unicode CRT initialization/argument parsing while
        # suppressing Windows' automatic console for ordinary GUI launch.
        set_target_properties(PixelBridgeEncoder PROPERTIES WIN32_EXECUTABLE TRUE qt_no_entrypoint TRUE)
        target_link_options(PixelBridgeEncoder PRIVATE /ENTRY:wmainCRTStartup)
        target_compile_options(PixelBridgeEncoder PRIVATE /external:anglebrackets /external:W0)
    endif()
    get_target_property(pbEncoderQtQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    if(pbEncoderQtQmake)
        get_filename_component(pbEncoderQtBin "${pbEncoderQtQmake}" DIRECTORY)
        find_program(pbEncoderWindeployqt NAMES windeployqt HINTS "${pbEncoderQtBin}" NO_DEFAULT_PATH)
        if(pbEncoderWindeployqt)
            add_custom_command(TARGET PixelBridgeEncoder POST_BUILD COMMAND "${pbEncoderWindeployqt}"
                --no-translations --no-opengl-sw "$<TARGET_FILE:PixelBridgeEncoder>" VERBATIM)
            # Offscreen is selected dynamically by --gui-smoke and is not
            # discovered by windeployqt's ordinary imports. Deploy exactly
            # this additional platform for non-interactive controller tests.
            get_filename_component(pbEncoderQtDirectory "${pbEncoderQtBin}" DIRECTORY)
            add_custom_command(TARGET PixelBridgeEncoder POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PixelBridgeEncoder>/platforms"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${pbEncoderQtDirectory}/plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                    "$<TARGET_FILE_DIR:PixelBridgeEncoder>/platforms/" VERBATIM)
        endif()
    endif()
    unset(pbEncoderGitCommit)
    unset(pbEncoderQtQmake)
    unset(pbEncoderQtBin)
    unset(pbEncoderQtDirectory)
    unset(pbEncoderWindeployqt)
endif()
