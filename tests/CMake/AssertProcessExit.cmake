# Exact exit-code contract for a tool process: the executable must exit
# with PB_EXPECTED_EXIT exactly (distinguishing exit 1 "evaluated but
# failed" from exit 2 "usage/IO error", which WILL_FAIL cannot do).
if(NOT DEFINED PB_TEST_EXECUTABLE OR PB_TEST_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "PB_TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED PB_EXPECTED_EXIT OR PB_EXPECTED_EXIT STREQUAL "")
    message(FATAL_ERROR "PB_EXPECTED_EXIT is required")
endif()
set(pbProcessArgs "")
if(DEFINED PB_TEST_ARGS_ENCODED)
    string(REPLACE "|" ";" pbProcessArgs "${PB_TEST_ARGS_ENCODED}")
elseif(DEFINED PB_TEST_ARGS)
    set(pbProcessArgs ${PB_TEST_ARGS})
endif()
execute_process(
    COMMAND "${PB_TEST_EXECUTABLE}" ${pbProcessArgs}
    RESULT_VARIABLE pbProcessResult
    OUTPUT_VARIABLE pbProcessStdout
    ERROR_VARIABLE pbProcessStderr)
if(NOT pbProcessResult STREQUAL "${PB_EXPECTED_EXIT}")
    message(FATAL_ERROR
        "${PB_TEST_EXECUTABLE} exited with ${pbProcessResult}, "
        "expected ${PB_EXPECTED_EXIT}\n"
        "stdout: ${pbProcessStdout}\n"
        "stderr: ${pbProcessStderr}")
endif()
