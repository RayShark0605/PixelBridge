add_library(PBApplication STATIC)
add_library(PB::PBApplication ALIAS PBApplication)

target_sources(PBApplication
    PRIVATE
        application_model.cpp
        decoder_resume_store.cpp
        encoder_session_store.cpp
        evidence_journal.cpp
        local_desktop_runtime.cpp
        optional_diagnostic_fanout.cpp
        optional_diagnostic_fanout.h
        monitor_catalog.cpp
        remote_visual_replay_recorder.cpp
        run_report.cpp
    PUBLIC
        application_model.h
        decoder_resume_store.h
        encoder_session_store.h
        evidence_journal.h
        local_desktop_runtime.h
        monitor_catalog.h
        remote_visual_replay_recorder.h
        run_report.h)

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
target_compile_definitions(PBApplication PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBApplication PRIVATE /external:anglebrackets /external:W0)
endif()
