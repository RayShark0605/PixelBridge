if(PB_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${PB_QT_ROOT}")
endif()
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

PbAddUnitTest(PBApplicationTests
    test_application_model.cpp
    test_offline_replay_runtime.cpp
    test_optional_diagnostic_fanout.cpp
    test_remote_visual_replay_recorder.cpp
    test_runtime_bindings.cpp
    test_session_persistence.cpp
    test_run_report.cpp)
target_link_libraries(PBApplicationTests PRIVATE PB::PBApplication PB::PBProtocol PB::PBCompression d3d11 dxgi)
target_compile_definitions(PBApplicationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_TEST_SCRATCH_ROOT=L\"${CMAKE_CURRENT_BINARY_DIR}\"")
PbInstrumentPresentationTest(PBApplicationTests)
# The headless production Replay corpus includes bounded WARP live/offline
# passes. Individual tests retain tighter explicit deadlines; this outer CTest
# bound prevents sanitizer overhead from terminating a still-progressing gate.
set_tests_properties(PBApplicationTests PROPERTIES LABELS "application;controller;gui-model" TIMEOUT 600)

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
        add_custom_command(TARGET PBQSettingsTests POST_BUILD COMMAND "${pbQSettingsWindeployqt}"
            --no-translations "$<TARGET_FILE:PBQSettingsTests>" VERBATIM)
    endif()
endif()
unset(pbQSettingsQmake)
unset(pbQSettingsQtBin)
unset(pbQSettingsWindeployqt)
