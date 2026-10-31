if(PB_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${PB_QT_ROOT}")
endif()
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)

PbAddUnitTest(PBSenderRatePriorTests test_sender_rate_prior.cpp "${PROJECT_SOURCE_DIR}/apps/common/sender_rate_prior.cpp")
target_include_directories(PBSenderRatePriorTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/common")
set_tests_properties(PBSenderRatePriorTests PROPERTIES LABELS "encoder;rate-prior;configuration" TIMEOUT 60)

PbAddUnitTest(PBPam4RatePriorRuntimeTests test_pam4_rate_prior_runtime.cpp test_rate_prior_reporting.cpp)
target_link_libraries(PBPam4RatePriorRuntimeTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBPam4RatePriorRuntimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBPam4RatePriorRuntimeTests PROPERTIES LABELS "encoder;pam4;rate-prior;recovery" TIMEOUT 120)

PbAddUnitTest(PBEncoderRatePriorOptionsTests test_encoder_rate_prior_options.cpp)
target_link_libraries(PBEncoderRatePriorOptionsTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Widgets)
target_include_directories(PBEncoderRatePriorOptionsTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder")
set_tests_properties(PBEncoderRatePriorOptionsTests PROPERTIES LABELS "encoder;rate-prior;gui;offscreen" TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

PbAddUnitTest(PBOperationalLoggingTests test_operational_logging.cpp
    "${PROJECT_SOURCE_DIR}/apps/common/operational_log_qt.cpp")
target_link_libraries(PBOperationalLoggingTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBOperationalLoggingTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE "PB_GIT_COMMIT=\"test\"")
set_tests_properties(PBOperationalLoggingTests PROPERTIES LABELS "logging;activity;gui-model" TIMEOUT 60)

PbAddUnitTest(PBDecoderSingleMonitorTests test_decoder_single_monitor.cpp)
target_link_libraries(PBDecoderSingleMonitorTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBDecoderSingleMonitorTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBDecoderSingleMonitorTests PROPERTIES LABELS "single-monitor;capture;nonlocal;pixels" TIMEOUT 120)

PbAddUnitTest(PBGuiVisualModeTests test_gui_visual_mode.cpp)
target_link_libraries(PBGuiVisualModeTests PRIVATE PB::PBApplication)
target_compile_definitions(PBGuiVisualModeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBGuiVisualModeTests PROPERTIES LABELS "gui-mode;configuration;nonlocal" TIMEOUT 30)

PbAddUnitTest(PBEncoderPresentationTests test_encoder_presentation.cpp)
target_link_libraries(PBEncoderPresentationTests PRIVATE PB::PBApplication)
target_include_directories(PBEncoderPresentationTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder")
target_sources(PBEncoderPresentationTests PRIVATE "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_compile_definitions(PBEncoderPresentationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBEncoderPresentationTests PROPERTIES LABELS "encoder;presentation;configuration" TIMEOUT 30)

PbAddUnitTest(PBQtRasterPresentationTests test_qt_raster_paint.cpp test_qt_raster_presentation.cpp test_qt_encoder_controller.cpp test_qt_raster_runtime.cpp qt_raster_test_support.cpp
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/qt_raster_paint.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/qt_raster_presentation.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/qt_raster_presentation.h"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/encoder_application_controller.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/encoder_application_controller.h"
    "${PROJECT_SOURCE_DIR}/apps/common/operational_log_qt.cpp")
set_target_properties(PBQtRasterPresentationTests PROPERTIES AUTOMOC ON)
target_link_libraries(PBQtRasterPresentationTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Widgets)
target_include_directories(PBQtRasterPresentationTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder"
    "${PROJECT_SOURCE_DIR}/libs/PBRenderD3D/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(PBQtRasterPresentationTests PRIVATE "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_compile_definitions(PBQtRasterPresentationTests PRIVATE PB_QT_RASTER_TESTS=1 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE "PB_GIT_COMMIT=\"test\""
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBQtRasterPresentationTests PROPERTIES LABELS "encoder;presentation;qt;offscreen" TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME PBQtRasterHiddenWindowTests COMMAND PBQtRasterPresentationTests "[.qt-hidden]")
set_tests_properties(PBQtRasterHiddenWindowTests PROPERTIES LABELS "encoder;presentation;qt;hidden" TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=windows")
foreach(scale IN ITEMS 1.25 1.5 2)
    add_test(NAME PBQtRasterHiddenDpr${scale}Tests COMMAND PBQtRasterPresentationTests "[.qt-hidden-dpr]")
    set_tests_properties(PBQtRasterHiddenDpr${scale}Tests PROPERTIES LABELS "encoder;presentation;qt;hidden;dpi-simulation" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCALE_FACTOR=${scale}")
endforeach()
get_target_property(pbRasterQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
get_filename_component(pbRasterQtBin "${pbRasterQmake}" DIRECTORY)
get_filename_component(pbRasterQtRoot "${pbRasterQtBin}" DIRECTORY)
find_program(pbRasterWindeployqt NAMES windeployqt HINTS "${pbRasterQtBin}" NO_DEFAULT_PATH REQUIRED)
add_custom_command(TARGET PBEncoderRatePriorOptionsTests POST_BUILD
    COMMAND "${pbRasterWindeployqt}" --no-translations --no-opengl-sw "$<TARGET_FILE:PBEncoderRatePriorOptionsTests>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PBEncoderRatePriorOptionsTests>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${pbRasterQtRoot}/plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
        "$<TARGET_FILE_DIR:PBEncoderRatePriorOptionsTests>/platforms/" VERBATIM)
add_custom_command(TARGET PBQtRasterPresentationTests POST_BUILD
    COMMAND "${pbRasterWindeployqt}" --no-translations --no-opengl-sw "$<TARGET_FILE:PBQtRasterPresentationTests>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PBQtRasterPresentationTests>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${pbRasterQtRoot}/plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
        "$<TARGET_FILE_DIR:PBQtRasterPresentationTests>/platforms/" VERBATIM)

PbAddUnitTest(PBExperimentalPam4WideApplicationTests test_experimental_pam4_wide_workflow.cpp)
target_link_libraries(PBExperimentalPam4WideApplicationTests PRIVATE PB::PBApplication)
target_compile_definitions(PBExperimentalPam4WideApplicationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBExperimentalPam4WideApplicationTests PROPERTIES LABELS "pam4-wide;experimental;application;pixels;file" TIMEOUT 120)

PbAddUnitTest(PBExperimentalPam4ApplicationTests test_experimental_pam4_workflow.cpp)
target_link_libraries(PBExperimentalPam4ApplicationTests PRIVATE PB::PBApplication)
target_compile_definitions(PBExperimentalPam4ApplicationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBExperimentalPam4ApplicationTests PROPERTIES LABELS "pam4;experimental;application;pixels;file" TIMEOUT 120)

# Compile only the resume store again with private native-I/O injection hooks.
# No test hook or policy-selection input enters PBApplication/product binaries.
PbAddUnitTest(PBDecoderResumeBatchTests test_decoder_resume_batch.cpp
    decoder_resume_coalesced_test_cases.inc decoder_resume_snapshot_test_hook.h
    "${PROJECT_SOURCE_DIR}/apps/common/decoder_resume_store.cpp")
target_link_libraries(PBDecoderResumeBatchTests PRIVATE PB::PBApplication)
target_include_directories(PBDecoderResumeBatchTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(PBDecoderResumeBatchTests PRIVATE PB_RESUME_BATCH_TESTS=1 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBDecoderResumeBatchTests PROPERTIES LABELS "resume;journal;batch;fault" TIMEOUT 120)

PbAddUnitTest(PBPersistenceAllocationFailureTests test_persistence_allocation_failure.cpp)
target_link_libraries(PBPersistenceAllocationFailureTests PRIVATE PB::PBApplication PB::PBStorage PB::PBProtocol)
target_compile_definitions(PBPersistenceAllocationFailureTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
PbInstrumentPresentationTest(PBPersistenceAllocationFailureTests)
set_tests_properties(PBPersistenceAllocationFailureTests PROPERTIES LABELS "storage;resume;allocation;cleanup" TIMEOUT 30)

PbAddUnitTest(PBApplicationTests
    test_application_model.cpp
    test_decoder_memory_budget.cpp
    test_encoder_monitor_catalog.cpp
    test_unified_encoder_workflow.cpp
    test_unified_decoder_workflow.cpp
    test_capture_fallback.cpp
    test_offline_replay_runtime.cpp
    test_optional_diagnostic_fanout.cpp
    test_remote_visual_replay_recorder.cpp
    test_runtime_bindings.cpp
    test_sender_streaming.cpp
    test_session_persistence.cpp
    test_run_report.cpp)
target_sources(PBApplicationTests PRIVATE test_unified_run_report.cpp)
target_sources(PBApplicationTests PRIVATE test_throughput_observation_report.cpp)
target_sources(PBApplicationTests PRIVATE test_step1_measurement.cpp)
target_sources(PBApplicationTests PRIVATE test_unified_bands_workflow.cpp)
target_sources(PBApplicationTests PRIVATE test_unified_graystates_workflow.cpp)
PbAddUnitTest(PBStep1GuiEvidenceTests test_step1_gui_evidence.cpp
    "${PROJECT_SOURCE_DIR}/apps/common/step1_gui_evidence_qt.cpp")
target_link_libraries(PBStep1GuiEvidenceTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBStep1GuiEvidenceTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBStep1GuiEvidenceTests PROPERTIES LABELS "step1;gui-model" TIMEOUT 30)
target_link_libraries(PBApplicationTests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_link_libraries(PBApplicationTests PRIVATE PB::PBApplication PB::PBProtocol PB::PBCompression d3d11 dxgi)
target_compile_definitions(PBApplicationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
PbInstrumentPresentationTest(PBApplicationTests)
# The headless production Replay corpus includes bounded WARP live/offline
# passes. Individual tests retain tighter explicit deadlines; this outer CTest
# bound prevents sanitizer overhead from terminating a still-progressing gate.
set_tests_properties(PBApplicationTests PROPERTIES LABELS "application;controller;gui-model" TIMEOUT 600)

PbAddUnitTest(PBEncoderShellTests
    test_encoder_shell.cpp
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/encoder_shell_integration.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder/folder_archive.cpp")
target_link_libraries(PBEncoderShellTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(PBEncoderShellTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/PixelBridgeEncoder")
target_compile_definitions(PBEncoderShellTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBEncoderShellTests PROPERTIES LABELS "application;encoder;shell;folder" TIMEOUT 60)

add_executable(PBHeadlessMultiSegmentCheckpoint headless_multi_segment_checkpoint.cpp)
target_link_libraries(PBHeadlessMultiSegmentCheckpoint PRIVATE PB::CompilerSettings PB::PBApplication
    PB::PBProtocol bcrypt)
target_compile_definitions(PBHeadlessMultiSegmentCheckpoint PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
target_compile_features(PBHeadlessMultiSegmentCheckpoint PRIVATE cxx_std_20)
add_test(NAME PBHeadlessMultiSegmentCheckpoint COMMAND PBHeadlessMultiSegmentCheckpoint)
set_tests_properties(PBHeadlessMultiSegmentCheckpoint PROPERTIES LABELS "application;headless;checkpoint"
    TIMEOUT 900)

PbAddUnitTest(PBUnifiedSenderSchedulerTests test_unified_sender_scheduler.cpp)
target_link_libraries(PBUnifiedSenderSchedulerTests PRIVATE PB::PBApplication PB::PBModulation PB::PBProtocol)
target_compile_definitions(PBUnifiedSenderSchedulerTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBUnifiedSenderSchedulerTests PROPERTIES LABELS "application;control;scheduler;transport;unified"
    TIMEOUT 120)

# Opt-in bounded 16-Segment Pass-0 evidence runner; never part of full CTest.
add_executable(PBUnifiedWindowTransitionProbe unified_window_transition_probe.cpp)
target_link_libraries(PBUnifiedWindowTransitionProbe PRIVATE PB::CompilerSettings PB::PBApplication PB::PBProtocol)
target_compile_definitions(PBUnifiedWindowTransitionProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

PbAddUnitTest(PBQSettingsTests test_qsettings_contract.cpp test_remote_visual_metadata_preset.cpp test_decoder_progress_text.cpp
    "${PROJECT_SOURCE_DIR}/apps/common/remote_visual_metadata_preset_qt.cpp")
target_link_libraries(PBQSettingsTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBQSettingsTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBQSettingsTests)
set_tests_properties(PBQSettingsTests PROPERTIES LABELS "application;qsettings;gui-model" TIMEOUT 30)

# No Qt, graphics or payload: bounded subprocess probes for GUI-subsystem CRT
# startup and inherited CLI handles. The external test hides its own console.
if(MSVC)
    foreach(probeTarget IN ITEMS PBGuiConsoleProbe PBConsoleParentProbe)
        add_executable(${probeTarget} application_console_probe.cpp)
        target_link_libraries(${probeTarget} PRIVATE PB::CompilerSettings)
        target_include_directories(${probeTarget} PRIVATE "${PROJECT_SOURCE_DIR}/apps/common")
        target_compile_definitions(${probeTarget} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endforeach()
    set_target_properties(PBGuiConsoleProbe PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(PBGuiConsoleProbe PRIVATE /ENTRY:wmainCRTStartup)
endif()

get_target_property(pbQSettingsQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
if(pbQSettingsQmake)
    get_filename_component(pbQSettingsQtBin "${pbQSettingsQmake}" DIRECTORY)
    find_program(pbQSettingsWindeployqt NAMES windeployqt HINTS "${pbQSettingsQtBin}" NO_DEFAULT_PATH)
    if(pbQSettingsWindeployqt)
        add_custom_command(TARGET PBApplicationTests POST_BUILD COMMAND "${pbQSettingsWindeployqt}"
            --no-translations "$<TARGET_FILE:PBApplicationTests>" VERBATIM)
        add_custom_command(TARGET PBStep1GuiEvidenceTests POST_BUILD COMMAND "${pbQSettingsWindeployqt}"
            --no-translations "$<TARGET_FILE:PBStep1GuiEvidenceTests>" VERBATIM)
        add_custom_command(TARGET PBQSettingsTests POST_BUILD COMMAND "${pbQSettingsWindeployqt}"
            --no-translations "$<TARGET_FILE:PBQSettingsTests>" VERBATIM)
    endif()
endif()
unset(pbQSettingsQmake)
unset(pbQSettingsQtBin)
unset(pbQSettingsWindeployqt)

# Explicit test-only clones: neither the ordinary PBApplication/PBStorage nor
# either product EXE receives a crash hook or the private recovery entry point.
option(PB_BUILD_PROCESS_RECOVERY_HARNESS "Build the opt-in G18/G19 process recovery worker" OFF)
if(PB_BUILD_PROCESS_RECOVERY_HARNESS)
    function(PbCloneProcessRecoveryLibrary targetName originalTarget)
        get_target_property(sourceDirectory ${originalTarget} SOURCE_DIR)
        get_target_property(sources ${originalTarget} SOURCES)
        set(absoluteSources)
        foreach(source IN LISTS sources)
            get_filename_component(absoluteSource "${source}" ABSOLUTE BASE_DIR "${sourceDirectory}")
            list(APPEND absoluteSources "${absoluteSource}")
        endforeach()
        list(REMOVE_DUPLICATES absoluteSources)
        add_library(${targetName} STATIC ${absoluteSources})
        foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FEATURES)
            get_target_property(value ${originalTarget} ${property})
            if(value)
                set_property(TARGET ${targetName} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        get_target_property(libraries ${originalTarget} LINK_LIBRARIES)
        list(TRANSFORM libraries REPLACE "^PB::PBStorage$" "PBG18Storage")
        target_link_libraries(${targetName} PUBLIC ${libraries})
        target_include_directories(${targetName} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(${targetName} PRIVATE PB_PROCESS_FAULT_TESTS=1)
        target_compile_features(${targetName} PRIVATE cxx_std_20)
    endfunction()

    PbCloneProcessRecoveryLibrary(PBG18Storage PBStorage)
    target_include_directories(PBG18Storage PUBLIC "${PROJECT_SOURCE_DIR}/libs/PBStorage/include")
    PbCloneProcessRecoveryLibrary(PBG18Application PBApplication)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE pbProcessTestBaseCommit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    target_compile_definitions(PBG18Application PRIVATE "PB_PROCESS_TEST_BASE_COMMIT=\"${pbProcessTestBaseCommit}\"")
    add_executable(PBProcessRecoveryWorker process_recovery_worker.cpp)
    target_link_libraries(PBProcessRecoveryWorker PRIVATE PB::CompilerSettings PBG18Application psapi rstrtmgr)
    target_compile_definitions(PBProcessRecoveryWorker PRIVATE PB_PROCESS_FAULT_TESTS=1
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    PbInstrumentPresentationTest(PBProcessRecoveryWorker)
    # Deliberately not registered in the default CTest suite. The serial Python
    # supervisor owns explicit fixture creation, process lifetime and evidence.
endif()
target_sources(PBApplicationTests PRIVATE test_step2_recording_contract.cpp)

PbAddUnitTest(PBFeedbackProtocolTests test_feedback_protocol.cpp)
target_link_libraries(PBFeedbackProtocolTests PRIVATE PB::PBApplication PB::PBProtocol)
target_compile_definitions(PBFeedbackProtocolTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBFeedbackProtocolTests PROPERTIES LABELS "application;feedback;protocol" TIMEOUT 30)

PbAddUnitTest(PBFeedbackPolicyTests test_feedback_policy.cpp)
target_link_libraries(PBFeedbackPolicyTests PRIVATE PB::PBApplication PB::PBProtocol)
target_compile_definitions(PBFeedbackPolicyTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBFeedbackPolicyTests PROPERTIES LABELS "application;feedback;policy" TIMEOUT 30)

PbAddUnitTest(PBFeedbackFileStoreTests test_feedback_file_store.cpp)
target_link_libraries(PBFeedbackFileStoreTests PRIVATE PB::PBApplication PB::PBProtocol)
target_compile_definitions(PBFeedbackFileStoreTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
set_tests_properties(PBFeedbackFileStoreTests PROPERTIES LABELS "application;feedback;file" TIMEOUT 30)

PbAddUnitTest(PBFeedbackIoProcessTests test_feedback_io_process.cpp)
target_link_libraries(PBFeedbackIoProcessTests PRIVATE PB::PBApplication PB::PBProtocol)
target_compile_definitions(PBFeedbackIoProcessTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\""
    "PB_FEEDBACK_HELPER_PATH=L\"${CMAKE_BINARY_DIR}/apps/PBFeedbackIo/$<CONFIG>/PBFeedbackIo.exe\""
    "PB_FEEDBACK_FIXTURE_PATH=L\"${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/PBFeedbackIoFixture.exe\"")
set_tests_properties(PBFeedbackIoProcessTests PROPERTIES LABELS "application;feedback;process" TIMEOUT 45)
add_executable(PBFeedbackIoFixture feedback_io_fixture.cpp)
set_target_properties(PBFeedbackIoFixture PROPERTIES AUTOMOC OFF)
target_link_libraries(PBFeedbackIoFixture PRIVATE PB::CompilerSettings PB::PBApplication)
target_compile_definitions(PBFeedbackIoFixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_dependencies(PBFeedbackIoProcessTests PBFeedbackIo PBFeedbackIoFixture)

PbAddUnitTest(PBFeedbackRuntimeTests test_feedback_runtime.cpp test_feedback_sender.cpp)
target_link_libraries(PBFeedbackRuntimeTests PRIVATE PB::PBApplication PB::PBProtocol PB::PBModulation)
target_compile_definitions(PBFeedbackRuntimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\""
    "PB_FEEDBACK_FIXTURE_PATH=L\"${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/PBFeedbackIoFixture.exe\"")
add_dependencies(PBFeedbackRuntimeTests PBFeedbackIo PBFeedbackIoFixture)
add_custom_command(TARGET PBFeedbackRuntimeTests POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:PBFeedbackIo>" "$<TARGET_FILE_DIR:PBFeedbackRuntimeTests>/PBFeedbackIo.exe" VERBATIM)
set_tests_properties(PBFeedbackRuntimeTests PROPERTIES LABELS "application;feedback;runtime" TIMEOUT 180)

PbAddUnitTest(PBFeedbackOptionsTests test_feedback_options.cpp
    "${PROJECT_SOURCE_DIR}/apps/common/feedback_options_qt.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/decoder_application_controller.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/decoder_application_controller.h"
    "${PROJECT_SOURCE_DIR}/apps/common/operational_log_qt.cpp")
set_target_properties(PBFeedbackOptionsTests PROPERTIES AUTOMOC ON)
target_link_libraries(PBFeedbackOptionsTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Widgets)
target_include_directories(PBFeedbackOptionsTests PRIVATE "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder")
target_compile_definitions(PBFeedbackOptionsTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE "PB_GIT_COMMIT=\"test\""
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
add_dependencies(PBFeedbackOptionsTests PBFeedbackIo)
add_custom_command(TARGET PBFeedbackOptionsTests POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:PBFeedbackIo>" "$<TARGET_FILE_DIR:PBFeedbackOptionsTests>/PBFeedbackIo.exe" VERBATIM)
set_tests_properties(PBFeedbackOptionsTests PROPERTIES LABELS "feedback;gui;offscreen;lifecycle" TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME PBFeedbackOptionsDpi150 COMMAND PBFeedbackOptionsTests)
set_tests_properties(PBFeedbackOptionsDpi150 PROPERTIES LABELS "feedback;gui;offscreen;dpi" TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5")
get_target_property(pbFeedbackQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
if(pbFeedbackQmake)
    get_filename_component(pbFeedbackQtBin "${pbFeedbackQmake}" DIRECTORY)
    find_program(pbFeedbackWindeployqt NAMES windeployqt HINTS "${pbFeedbackQtBin}" NO_DEFAULT_PATH)
    if(pbFeedbackWindeployqt)
        get_filename_component(pbFeedbackQtRoot "${pbFeedbackQtBin}" DIRECTORY)
        add_custom_command(TARGET PBFeedbackOptionsTests POST_BUILD COMMAND "${pbFeedbackWindeployqt}"
            --no-translations "$<TARGET_FILE:PBFeedbackOptionsTests>"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PBFeedbackOptionsTests>/platforms"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${pbFeedbackQtRoot}/plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:PBFeedbackOptionsTests>/platforms/" VERBATIM)
    endif()
endif()
unset(pbFeedbackQmake)
unset(pbFeedbackQtBin)
unset(pbFeedbackWindeployqt)
unset(pbFeedbackQtRoot)

PbAddUnitTest(PBFeedbackReportTests test_feedback_report.cpp)
target_link_libraries(PBFeedbackReportTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBFeedbackReportTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_tests_properties(PBFeedbackReportTests PROPERTIES LABELS "feedback;report;privacy" TIMEOUT 30)
