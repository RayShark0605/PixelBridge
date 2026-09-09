add_library(PBApplication STATIC)
add_library(PB::PBApplication ALIAS PBApplication)

target_sources(PBApplication
    PRIVATE
        application_model.cpp
        decoder_capture_controller.cpp
        decoder_resume_store.cpp
        encoder_session_store.cpp
        encoder_monitor_catalog.cpp
        evidence_journal.cpp
        local_desktop_runtime.cpp
        optional_diagnostic_fanout.cpp
        optional_diagnostic_fanout.h
        monitor_catalog.cpp
        remote_visual_replay_recorder.cpp
        run_report.cpp
        run_measurement.cpp
        recorded_pixel_replay.cpp
        sender_carousel_scheduler.cpp
    PUBLIC
        application_model.h
        decoder_capture_controller.h
        decoder_resume_store.h
        encoder_session_store.h
        encoder_monitor_catalog.h
        evidence_journal.h
        local_desktop_runtime.h
        monitor_catalog.h
        remote_visual_replay_recorder.h
        run_report.h
        run_measurement.h
        sender_carousel_scheduler.h)

target_include_directories(PBApplication PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(PBApplication
    PRIVATE
        PB::CompilerSettings
    PUBLIC
        PB::PBCore
        PB::PBProtocol
        PB::PBCompression
        PB::PBOuterFec
        PB::PBInnerFec
        PB::PBReceiver
        PB::PBStorage
        PB::PBRenderD3D
        PB::PBScreenRegion
        PB::PBScreenCaptureWgc
        PB::PBScreenCaptureDxgi
        PB::PBCaptureNormalize
        PB::PBDemodD3D11
        PB::PBModulation
        PB::PBDesktopLevelsReference
        PB::PBPresentTiming
        PB::PBTelemetry)

target_link_libraries(PBApplication PUBLIC PB::PBRealCaptureReplay)

target_link_libraries(PBApplication PRIVATE Shcore d3d11 dxgi)

target_compile_features(PBApplication PUBLIC cxx_std_20)
set(PB_STEP1_SOURCE_FINGERPRINT "unsealed" CACHE STRING "Sealed Step1 experimental source fingerprint")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE pbStep1BaseCommit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(PBApplication PRIVATE
    "PB_STEP1_SOURCE_FINGERPRINT=\"${PB_STEP1_SOURCE_FINGERPRINT}\""
    "PB_STEP1_BASE_COMMIT=\"${pbStep1BaseCommit}\"")
unset(pbStep1BaseCommit)
target_compile_definitions(PBApplication PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBApplication PRIVATE /external:anglebrackets /external:W0)
endif()
