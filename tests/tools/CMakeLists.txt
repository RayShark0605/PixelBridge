# Catch2 suites for the command-line tool cores (PBProtocolDump /
# PBFrameInspector). The core targets are defined in tools/CMakeLists.txt,
# which exists only when PB_BUILD_TOOLS=ON; tests/CMakeLists.txt therefore
# adds this directory only when the core targets exist.
#
# All fixtures are synthesized in memory: record byte vectors come from the
# deterministic PBGoldenVector byte source, frames from
# pbmodulation::EncodeReferenceFrame plus the PBInnerFec / PBProtocol
# encoders. The only on-disk tool fixture is the 28-byte hostile PBRW header
# used by the process-level pre-allocation rejection test.

PbAddUnitTest(PBProtocolDumpTests
    test_protocol_dump_core.cpp)
target_link_libraries(PBProtocolDumpTests
    PRIVATE
        PBProtocolDumpCore
        PB::PBGoldenVector
        PB::PBModulation
        PB::PBProtocol)

PbAddUnitTest(PBFrameInspectorTests
    test_frame_inspector_core.cpp)
target_link_libraries(PBFrameInspectorTests
    PRIVATE
        PBFrameInspectorCore
        PB::PBGoldenVector
        PB::PBInnerFec
        PB::PBModulation
        PB::PBProtocol)
target_include_directories(PBFrameInspectorTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/PBModulation")

PbAddUnitTest(PBRemoteVisualChannelMatrixTests
    test_remote_visual_channel_matrix.cpp)
target_link_libraries(PBRemoteVisualChannelMatrixTests
    PRIVATE
        PBRemoteVisualChannelMatrixCore
        PB::PBRemoteVisualSimulator)
PbInstrumentPresentationTest(PBRemoteVisualChannelMatrixTests)
set_tests_properties(PBRemoteVisualChannelMatrixTests PROPERTIES
    LABELS "tools;remote-visual;simulator;matrix;truth"
    TIMEOUT 300)

PbAddUnitTest(PBRemoteVisualReceiverEvidenceTests
    test_remote_visual_receiver_evidence.cpp)
target_link_libraries(PBRemoteVisualReceiverEvidenceTests
    PRIVATE
        PBRemoteVisualReceiverEvidenceCore)
PbInstrumentPresentationTest(PBRemoteVisualReceiverEvidenceTests)
set_tests_properties(PBRemoteVisualReceiverEvidenceTests PROPERTIES
    LABELS "tools;remote-visual;receiver;outer;digest;resource"
    TIMEOUT 300)

PbAddUnitTest(PBRemoteVisualCodecProbeTests
    test_remote_visual_codec_probe.cpp)
target_link_libraries(PBRemoteVisualCodecProbeTests
    PRIVATE
        PBRemoteVisualCodecProbeCore)
PbInstrumentPresentationTest(PBRemoteVisualCodecProbeTests)
set_tests_properties(PBRemoteVisualCodecProbeTests PROPERTIES
    LABELS "tools;remote-visual;codec;fec;transport;resource"
    TIMEOUT 300)

PbAddUnitTest(PBRemoteVisualReplayInspectorTests
    test_remote_visual_replay_inspector.cpp)
target_link_libraries(PBRemoteVisualReplayInspectorTests
    PRIVATE
        PBRemoteVisualReplayInspectorCore
        PB::PBModulation
        PB::PBProtocol)
PbInstrumentPresentationTest(PBRemoteVisualReplayInspectorTests)
set_tests_properties(PBRemoteVisualReplayInspectorTests PROPERTIES
    LABELS "tools;remote-visual;replay;receiver-only;transport;determinism"
    TIMEOUT 300)

PbAddUnitTest(PBRemoteVisualMetricCalibrationTests
    test_remote_visual_metric_calibration.cpp)
target_link_libraries(PBRemoteVisualMetricCalibrationTests
    PRIVATE
        PBRemoteVisualMetricCalibrationCore
        PB::PBModulation
        PB::PBProtocol)
PbInstrumentPresentationTest(PBRemoteVisualMetricCalibrationTests)
set_tests_properties(PBRemoteVisualMetricCalibrationTests PROPERTIES
    LABELS "tools;remote-visual;calibration;holdout;truth;transport"
    TIMEOUT 300)

if(TARGET PBRemoteVisualTemporalCorpusCore)
    PbAddUnitTest(PBRemoteVisualTemporalCorpusTests
        test_remote_visual_temporal_corpus.cpp)
    target_link_libraries(PBRemoteVisualTemporalCorpusTests
        PRIVATE
            PBRemoteVisualTemporalCorpusCore)
    PbInstrumentPresentationTest(PBRemoteVisualTemporalCorpusTests)
    set_tests_properties(PBRemoteVisualTemporalCorpusTests PROPERTIES
        LABELS "tools;remote-visual;temporal;identity;fec;transport"
        TIMEOUT 300)

    add_test(NAME PBRemoteVisualTemporalCorpusIntegration
        COMMAND ${CMAKE_COMMAND}
            "-DPB_TEMPORAL_CORPUS_EXE=$<TARGET_FILE:PBRemoteVisualTemporalCorpus>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRemoteVisualTemporalCorpus.cmake")
    set_tests_properties(PBRemoteVisualTemporalCorpusIntegration PROPERTIES
        LABELS "tools;remote-visual;temporal;identity;integration"
        TIMEOUT 300)
endif()

# CLI exit-code contracts for the tool executables. The positive input
# is the committed golden bootstrap record (exit 0); the parse-failure input
# is the committed bootstrap-control corpus negative sample (exit 1); a
# missing file is a usage/IO error (exit 2). The frame-inspector hostile
# header is a committed 28-byte PBRW fixture declaring 16384x16384; the
# 8.3 MB positive frame path is covered by the in-memory suite.
if(TARGET PBProtocolDump)
    add_test(
        NAME PBProtocolDumpExit.Valid
        COMMAND PBProtocolDump
            "${PROJECT_SOURCE_DIR}/tests/golden/protocol/bootstrap-record.bin")
    set_tests_properties(PBProtocolDumpExit.Valid PROPERTIES
        LABELS "tools;exit-codes"
        TIMEOUT 30)

    add_test(
        NAME PBProtocolDumpExit.ParseFailure
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBProtocolDump>
            -DPB_EXPECTED_EXIT=1
            -DPB_TEST_ARGS=${PROJECT_SOURCE_DIR}/fuzz/corpus/bootstrap-control/bad-bootstrap-crc.bin
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBProtocolDumpExit.ParseFailure PROPERTIES
        LABELS "tools;exit-codes"
        TIMEOUT 30)

    add_test(
        NAME PBProtocolDumpExit.UsageError
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBProtocolDump>
            -DPB_EXPECTED_EXIT=2
            -DPB_TEST_ARGS=${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.bin
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBProtocolDumpExit.UsageError PROPERTIES
        LABELS "tools;exit-codes"
        TIMEOUT 30)
endif()

if(TARGET PBRemoteVisualChannelMatrix)
    add_test(
        NAME PBRemoteVisualChannelMatrixIntegration
        COMMAND "${CMAKE_COMMAND}"
            -DPB_MATRIX_TOOL=$<TARGET_FILE:PBRemoteVisualChannelMatrix>
            -DPB_WORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/remote-visual-channel-matrix-integration
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRemoteVisualChannelMatrix.cmake")
    set_tests_properties(PBRemoteVisualChannelMatrixIntegration PROPERTIES
        LABELS "tools;remote-visual;simulator;matrix;truth;file-safety"
        TIMEOUT 120)

    add_test(
        NAME PBRemoteVisualChannelMatrixExit.UsageError
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBRemoteVisualChannelMatrix>
            -DPB_EXPECTED_EXIT=2
            -DPB_TEST_ARGS_ENCODED=--output
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBRemoteVisualChannelMatrixExit.UsageError PROPERTIES
        LABELS "tools;remote-visual;exit-codes"
        TIMEOUT 30)
endif()

if(TARGET PBRemoteVisualEvidencePresenter)
    foreach(pbEvidenceProfile IN ITEMS direct shape lf4)
        add_test(
            NAME "PBRemoteVisualEvidencePresenterDescribe.${pbEvidenceProfile}"
            COMMAND PBRemoteVisualEvidencePresenter describe --profile ${pbEvidenceProfile})
        set_tests_properties("PBRemoteVisualEvidencePresenterDescribe.${pbEvidenceProfile}" PROPERTIES
            LABELS "tools;remote-visual;evidence;sender;determinism"
            PASS_REGULAR_EXPRESSION "PixelBridge.RemoteVisualEvidenceRaster.1"
            TIMEOUT 30)
    endforeach()
    unset(pbEvidenceProfile)

    add_test(
        NAME PBRemoteVisualEvidencePresenterExit.InvalidProfile
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBRemoteVisualEvidencePresenter>
            -DPB_EXPECTED_EXIT=2
            -DPB_TEST_ARGS_ENCODED=describe|--profile|unknown
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBRemoteVisualEvidencePresenterExit.InvalidProfile PROPERTIES
        LABELS "tools;remote-visual;evidence;exit-codes"
        TIMEOUT 30)
endif()

if(TARGET PBRemoteVisualLf4DynamicPresenter)
    add_test(
        NAME PBRemoteVisualLf4DynamicPresenterDescribe
        COMMAND PBRemoteVisualLf4DynamicPresenter describe)
    set_tests_properties(PBRemoteVisualLf4DynamicPresenterDescribe PROPERTIES
        LABELS "tools;remote-visual;evidence;sender;dynamic;production-runtime"
        PASS_REGULAR_EXPRESSION "PixelBridge.RemoteVisualLf4DynamicPresenter.1.*productionEncoderRuntime.*dynamicCarousel"
        TIMEOUT 30)

    add_test(NAME PBRemoteVisualLf4DynamicPresenterExit.MissingSource
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBRemoteVisualLf4DynamicPresenter>
            -DPB_EXPECTED_EXIT=1
            -DPB_TEST_ARGS_ENCODED=broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBRemoteVisualLf4DynamicPresenterExit.MissingSource PROPERTIES
        LABELS "tools;remote-visual;evidence;sender;dynamic;validation"
        TIMEOUT 30)

    set(pbLf4DynamicPresenterInvalidArguments
        ""
        "unknown"
        "describe|extra"
        "broadcast"
        "broadcast|--source|missing.bin|--origin|0|0"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--experiment-monitor|experiment|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|same|--experiment-monitor|same|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|1"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|601"
        "broadcast|--source|missing.bin|--origin|+1|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2"
        "broadcast|--source|missing.bin|--origin|2147483648|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2|--logical-fps|0"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2|--logical-fps|6"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2|--control-repetitions|0"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2|--control-repetitions|65"
        "broadcast|--source|missing.bin|--source|other.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--protected-monitor|other|--experiment-monitor|experiment|--seconds|2"
        "broadcast|--source|missing.bin|--origin|0|0|--protected-monitor|protected|--experiment-monitor|experiment|--experiment-monitor|other|--seconds|2")
    set(pbLf4DynamicPresenterArgumentIndex 0)
    foreach(pbLf4DynamicPresenterArguments IN LISTS pbLf4DynamicPresenterInvalidArguments)
        add_test(NAME "PBRemoteVisualLf4DynamicPresenterExit.Invalid.${pbLf4DynamicPresenterArgumentIndex}"
            COMMAND "${CMAKE_COMMAND}"
                -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBRemoteVisualLf4DynamicPresenter>
                -DPB_EXPECTED_EXIT=2
                -DPB_TEST_ARGS_ENCODED=${pbLf4DynamicPresenterArguments}
                -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
        set_tests_properties("PBRemoteVisualLf4DynamicPresenterExit.Invalid.${pbLf4DynamicPresenterArgumentIndex}" PROPERTIES
            LABELS "tools;remote-visual;evidence;sender;dynamic;exit-codes"
            TIMEOUT 30)
        math(EXPR pbLf4DynamicPresenterArgumentIndex "${pbLf4DynamicPresenterArgumentIndex} + 1")
    endforeach()
    unset(pbLf4DynamicPresenterInvalidArguments)
    unset(pbLf4DynamicPresenterArgumentIndex)
    unset(pbLf4DynamicPresenterArguments)
endif()

if(TARGET PBFrameInspector)
    set(pbHostileHeaderFile
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hostile-16384.pbrw")

    add_test(
        NAME PBFrameInspectorExit.HostileGeometry
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBFrameInspector>
            -DPB_EXPECTED_EXIT=1
            -DPB_TEST_ARGS=${pbHostileHeaderFile}
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBFrameInspectorExit.HostileGeometry PROPERTIES
        LABELS "tools;exit-codes"
        TIMEOUT 30)

    add_test(
        NAME PBFrameInspectorExit.UsageError
        COMMAND "${CMAKE_COMMAND}"
            -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBFrameInspector>
            -DPB_EXPECTED_EXIT=2
            -DPB_TEST_ARGS=${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.pbrw
            -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
    set_tests_properties(PBFrameInspectorExit.UsageError PROPERTIES
        LABELS "tools;exit-codes"
        TIMEOUT 30)
    unset(pbHostileHeaderFile)
endif()

if(TARGET PBVectorGen AND TARGET PBFrameInspector)
    add_test(
        NAME PBVectorGenIntegration
        COMMAND "${CMAKE_COMMAND}"
            -DPB_VECTOR_GEN=$<TARGET_FILE:PBVectorGen>
            -DPB_FRAME_INSPECTOR=$<TARGET_FILE:PBFrameInspector>
            -DPB_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -DPB_WORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/vector-gen-integration
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RunVectorGenIntegration.cmake")
    set_tests_properties(PBVectorGenIntegration PROPERTIES
        LABELS "tools;generator;integration"
        TIMEOUT 180)

    foreach(pbVectorGenInvalidCase IN ITEMS
            UnknownCategory
            DuplicateCategory
            MultipleRoots
            InvalidSeed
            OverflowSeed
            MissingValue
            ExtraArgument)
        if(pbVectorGenInvalidCase STREQUAL "UnknownCategory")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid --category unknown)
        elseif(pbVectorGenInvalidCase STREQUAL "DuplicateCategory")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid --category transport --category ldpc)
        elseif(pbVectorGenInvalidCase STREQUAL "MultipleRoots")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid ${CMAKE_CURRENT_BINARY_DIR}/extra --category transport)
        elseif(pbVectorGenInvalidCase STREQUAL "InvalidSeed")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid --category transport --seed nope)
        elseif(pbVectorGenInvalidCase STREQUAL "OverflowSeed")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid --category transport --seed 18446744073709551616)
        elseif(pbVectorGenInvalidCase STREQUAL "MissingValue")
            set(pbVectorGenInvalidArguments write-corpus ${CMAKE_CURRENT_BINARY_DIR}/invalid --category)
        else()
            set(pbVectorGenInvalidArguments dump-manifest --skip-frames extra)
        endif()
        string(REPLACE ";" "|" pbVectorGenInvalidArgumentsEncoded "${pbVectorGenInvalidArguments}")
        add_test(
            NAME "PBVectorGenExit.${pbVectorGenInvalidCase}"
            COMMAND "${CMAKE_COMMAND}"
                -DPB_TEST_EXECUTABLE=$<TARGET_FILE:PBVectorGen>
                -DPB_EXPECTED_EXIT=2
                -DPB_TEST_ARGS_ENCODED=${pbVectorGenInvalidArgumentsEncoded}
                -P "${PROJECT_SOURCE_DIR}/tests/CMake/AssertProcessExit.cmake")
        set_tests_properties("PBVectorGenExit.${pbVectorGenInvalidCase}" PROPERTIES
            LABELS "tools;generator;exit-codes"
            TIMEOUT 30)
    endforeach()
    unset(pbVectorGenInvalidArgumentsEncoded)
    unset(pbVectorGenInvalidArguments)
    unset(pbVectorGenInvalidCase)
endif()

if(WIN32)
    find_program(PB_REMOTE_VISUAL_POWERSHELL NAMES pwsh REQUIRED)
    find_program(PB_REMOTE_VISUAL_EVIDENCE_PYTHON
        NAMES python.exe
        PATHS "D:/Python3.12.9"
        NO_DEFAULT_PATH
        REQUIRED)
    add_test(NAME PBRemoteVisualDeploymentEvidenceContracts
        COMMAND "${PB_REMOTE_VISUAL_POWERSHELL}"
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRemoteVisualDeploymentEvidence.ps1"
            -ToolsRoot "${PROJECT_SOURCE_DIR}/tools/PBRemoteVisualEvidence"
            -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/remote-visual-deployment-evidence")
    set_tests_properties(PBRemoteVisualDeploymentEvidenceContracts PROPERTIES
        LABELS "tools;remote-visual;deployment;package;source-set;file-safety"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    add_test(NAME PBLocalDesktopRegressionEvidenceContracts
        COMMAND "${PB_REMOTE_VISUAL_POWERSHELL}"
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VerifyLocalDesktopRegressionEvidence.ps1"
            -ToolsRoot "${PROJECT_SOURCE_DIR}/tools/PBRemoteVisualEvidence"
            -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/local-desktop-regression-evidence")
    set_tests_properties(PBLocalDesktopRegressionEvidenceContracts PROPERTIES
        LABELS "tools;remote-visual;local-desktop;regression;evidence;file-safety"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    add_test(NAME PBRemoteVisualPilotEvidenceContracts
        COMMAND "${PB_REMOTE_VISUAL_POWERSHELL}"
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRemoteVisualPilotEvidence.ps1"
            -ToolsRoot "${PROJECT_SOURCE_DIR}/tools/PBRemoteVisualEvidence"
            -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/remote-visual-pilot-evidence"
            -PythonExecutable "${PB_REMOTE_VISUAL_EVIDENCE_PYTHON}")
    set_tests_properties(PBRemoteVisualPilotEvidenceContracts PROPERTIES
        LABELS "tools;remote-visual;pilot;remote-ui;plan;resource;evidence;file-safety"
        RUN_SERIAL TRUE
        TIMEOUT 120)

    add_test(NAME PBRemoteVisualStep21MatrixEvidenceContracts
        COMMAND "${PB_REMOTE_VISUAL_POWERSHELL}"
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRemoteVisualStep21MatrixEvidence.ps1"
            -ToolsRoot "${PROJECT_SOURCE_DIR}/tools/PBRemoteVisualEvidence"
            -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/remote-visual-step21-matrix-evidence")
    set_tests_properties(PBRemoteVisualStep21MatrixEvidenceContracts PROPERTIES
        LABELS "tools;remote-visual;step21;matrix;failure-classification;evidence;file-safety"
        RUN_SERIAL TRUE
        TIMEOUT 180)
endif()

# Step 01 freezes the annotated "phase1-gate-pass" tag as the boundary between the Phase-1 Gate
# evidence and the low-FPS RemoteVisual route, and forbids moving or recreating it. The identity is
# therefore re-checked by the suite instead of by a human running git, and the same script runs in
# the portable-package tool before tag identity is written into a package manifest.
find_package(Git QUIET)
add_test(NAME PBPhase1GateTagIdentity
    COMMAND "${CMAKE_COMMAND}"
        "-DPB_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}"
        "-DPB_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${PROJECT_SOURCE_DIR}/cmake/PBVerifyPhase1GateTagIdentity.cmake")
set_tests_properties(PBPhase1GateTagIdentity PROPERTIES
    LABELS "tools;remote-visual;baseline;gate-validation"
    TIMEOUT 120)
