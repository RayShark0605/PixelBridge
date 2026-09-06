if(PB_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${PB_QT_ROOT}")
endif()
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

PbAddUnitTest(PBApplicationTests
    test_application_model.cpp
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
target_link_libraries(PBApplicationTests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_link_libraries(PBApplicationTests PRIVATE PB::PBApplication PB::PBProtocol PB::PBCompression d3d11 dxgi)
target_compile_definitions(PBApplicationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
PbInstrumentPresentationTest(PBApplicationTests)
# The headless production Replay corpus includes bounded WARP live/offline
# passes. Individual tests retain tighter explicit deadlines; this outer CTest
# bound prevents sanitizer overhead from terminating a still-progressing gate.
set_tests_properties(PBApplicationTests PROPERTIES LABELS "application;controller;gui-model" TIMEOUT 600)

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

PbAddUnitTest(PBQSettingsTests test_qsettings_contract.cpp test_remote_visual_metadata_preset.cpp
    "${PROJECT_SOURCE_DIR}/apps/common/remote_visual_metadata_preset_qt.cpp")
target_link_libraries(PBQSettingsTests PRIVATE PB::PBApplication Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(PBQSettingsTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBQSettingsTests)
set_tests_properties(PBQSettingsTests PROPERTIES LABELS "application;qsettings;gui-model" TIMEOUT 30)
get_target_property(pbQSettingsQmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
if(pbQSettingsQmake)
    get_filename_component(pbQSettingsQtBin "${pbQSettingsQmake}" DIRECTORY)
    find_program(pbQSettingsWindeployqt NAMES windeployqt HINTS "${pbQSettingsQtBin}" NO_DEFAULT_PATH)
    if(pbQSettingsWindeployqt)
        add_custom_command(TARGET PBApplicationTests POST_BUILD COMMAND "${pbQSettingsWindeployqt}"
            --no-translations "$<TARGET_FILE:PBApplicationTests>" VERBATIM)
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
