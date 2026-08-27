add_executable(PBPhase0Gate phase0_gate_main.cpp reference_frame_receiver.cpp reference_frame_tests.cpp)

target_link_libraries(PBPhase0Gate
    PRIVATE
        PB::CompilerSettings
        PB::PBCompression
        PB::PBInnerFec
        PB::PBModulation
        PB::PBOuterFec
        PB::PBProtocol
        PB::PBReceiver)

if(MSVC)
    target_compile_options(PBPhase0Gate
        PRIVATE
            /external:anglebrackets
            /external:W0)
endif()

if(PB_BUILD_FUZZERS AND MSVC)
    target_compile_options(PBPhase0Gate PRIVATE /Zi /fsanitize=address)
    target_compile_definitions(PBPhase0Gate
        PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    target_link_options(PBPhase0Gate PRIVATE /DEBUG /INCREMENTAL:NO)
elseif(PB_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(PBPhase0Gate
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(PBPhase0Gate PRIVATE -fsanitize=address,undefined)
endif()

set(pbPhase0ScratchRoot "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")

add_test(NAME PBPhase0GateFast
    COMMAND PBPhase0Gate
        --mode fast
        --scratch "${pbPhase0ScratchRoot}/fast"
        --evidence "${pbPhase0ScratchRoot}/phase0-fast.jsonl")
set_tests_properties(PBPhase0GateFast PROPERTIES
    LABELS "phase0-gate"
    TIMEOUT 1800)

add_test(NAME PBPhase0ScratchSafety
    COMMAND "${CMAKE_COMMAND}"
        "-DPB_GATE_EXECUTABLE=$<TARGET_FILE:PBPhase0Gate>"
        "-DPB_SCRATCH_ROOT=${pbPhase0ScratchRoot}/scratch-safety"
        "-DPB_EVIDENCE_PATH=${pbPhase0ScratchRoot}/phase0-scratch-safety.jsonl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunScratchSafetyGate.cmake")
set_tests_properties(PBPhase0ScratchSafety PROPERTIES
    LABELS "phase0-gate"
    TIMEOUT 60)

add_test(NAME PBPhase0GateLarge
    COMMAND PBPhase0Gate
        --mode large
        --scratch "${pbPhase0ScratchRoot}/large"
        --evidence "${pbPhase0ScratchRoot}/phase0-large.jsonl"
    CONFIGURATIONS Release)
set_tests_properties(PBPhase0GateLarge PROPERTIES
    LABELS "phase0-large"
    TIMEOUT 7200)

add_test(NAME PBPhase0GateResume
    COMMAND PBPhase0Gate
        --mode resume
        --scratch "${pbPhase0ScratchRoot}/resume"
        --evidence "${pbPhase0ScratchRoot}/phase0-resume.jsonl")
set_tests_properties(PBPhase0GateResume PROPERTIES
    LABELS "phase0-resume"
    TIMEOUT 1800)

add_test(NAME PBPhase0ResumeRegressions
    COMMAND "${CMAKE_COMMAND}"
        "-DPB_RECEIVER_TEST=$<TARGET_FILE:PBReceiverTests>"
        "-DPB_EVIDENCE_PATH=${pbPhase0ScratchRoot}/phase0-resume-regressions.jsonl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunResumeGate.cmake")
set_tests_properties(PBPhase0ResumeRegressions PROPERTIES
    LABELS "phase0-resume"
    # Both invocations use PBReceiverTests' per-case file-IO scratch roots.
    # Preserve every assertion while preventing overlap inside one CTest run.
    DEPENDS PBReceiverTests
    TIMEOUT 600)

# The shared libraries are ASan-instrumented in the report-grade fuzz tree.
# Register this plain executable with the centralized runtime-copy target.
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBPhase0Gate)

unset(pbPhase0ScratchRoot)
