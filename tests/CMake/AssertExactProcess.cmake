if(NOT DEFINED PB_TEST_EXECUTABLE OR PB_TEST_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "PB_TEST_EXECUTABLE is required")
endif()

if(NOT DEFINED PB_EXPECTED_STDOUT)
    message(FATAL_ERROR "PB_EXPECTED_STDOUT is required")
endif()

set(pbExactProcessArgs "")
if(DEFINED PB_TEST_ARGS_ENCODED)
    string(REPLACE "|" ";" pbExactProcessArgs "${PB_TEST_ARGS_ENCODED}")
endif()

execute_process(
    COMMAND "${PB_TEST_EXECUTABLE}" ${pbExactProcessArgs}
    RESULT_VARIABLE processResult
    OUTPUT_VARIABLE processStdout
    ERROR_VARIABLE processStderr)

if(NOT processResult STREQUAL "0")
    message(FATAL_ERROR
        "${PB_TEST_EXECUTABLE} exited with ${processResult}\n"
        "stdout: ${processStdout}\n"
        "stderr: ${processStderr}")
endif()

unset(pbExactProcessArgs)

if(NOT processStderr STREQUAL "")
    message(FATAL_ERROR
        "${PB_TEST_EXECUTABLE} wrote unexpected stderr: ${processStderr}")
endif()

string(REPLACE "\r\n" "\n" normalizedStdout "${processStdout}")
set(expectedStdoutWithNewline "${PB_EXPECTED_STDOUT}\n")
if(NOT normalizedStdout STREQUAL expectedStdoutWithNewline)
    message(FATAL_ERROR
        "Unexpected stdout from ${PB_TEST_EXECUTABLE}\n"
        "expected: [${expectedStdoutWithNewline}]\n"
        "actual:   [${normalizedStdout}]")
endif()
