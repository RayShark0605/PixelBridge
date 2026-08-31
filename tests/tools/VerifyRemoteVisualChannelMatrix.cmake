if(NOT DEFINED PB_MATRIX_TOOL OR NOT DEFINED PB_WORK_ROOT)
    message(FATAL_ERROR "PB_MATRIX_TOOL and PB_WORK_ROOT are required")
endif()

file(MAKE_DIRECTORY "${PB_WORK_ROOT}")
set(pbMatrixOutput "${PB_WORK_ROOT}/remote-visual-channel-matrix.json")
set(pbMatrixPartial "${pbMatrixOutput}.partial")
file(REMOVE "${pbMatrixOutput}" "${pbMatrixPartial}")

execute_process(
    COMMAND "${PB_MATRIX_TOOL}" --output "${pbMatrixOutput}"
    RESULT_VARIABLE pbMatrixResult
    OUTPUT_VARIABLE pbMatrixStdout
    ERROR_VARIABLE pbMatrixStderr)
if(NOT pbMatrixResult EQUAL 0)
    message(FATAL_ERROR "matrix generation failed: exit=${pbMatrixResult} stderr=${pbMatrixStderr}")
endif()
if(NOT pbMatrixStdout STREQUAL "")
    message(FATAL_ERROR "file mode unexpectedly wrote stdout: ${pbMatrixStdout}")
endif()
if(NOT EXISTS "${pbMatrixOutput}" OR EXISTS "${pbMatrixPartial}")
    message(FATAL_ERROR "matrix output was not published atomically")
endif()

file(READ "${pbMatrixOutput}" pbMatrixJson)
string(JSON pbMatrixSchema ERROR_VARIABLE pbMatrixJsonError GET "${pbMatrixJson}" schema)
if(pbMatrixJsonError OR NOT pbMatrixSchema STREQUAL "PixelBridge.RemoteVisualChannelMatrix.1")
    message(FATAL_ERROR "unexpected matrix schema: ${pbMatrixJsonError} ${pbMatrixSchema}")
endif()
string(JSON pbMatrixCaseCount GET "${pbMatrixJson}" payload summary caseCount)
string(JSON pbMatrixFalseAccepted GET "${pbMatrixJson}" payload summary falseAcceptedCodewords)
string(JSON pbMatrixTruthBoundary GET "${pbMatrixJson}" payload summary truthBoundaryValid)
string(JSON pbMatrixExpectations GET "${pbMatrixJson}" payload summary expectationsMatched)
if(NOT pbMatrixCaseCount EQUAL 15 OR NOT pbMatrixFalseAccepted EQUAL 0 OR
    NOT pbMatrixTruthBoundary OR NOT pbMatrixExpectations)
    message(FATAL_ERROR
        "unexpected matrix summary: cases=${pbMatrixCaseCount} falseAccepted=${pbMatrixFalseAccepted} "
        "truth=${pbMatrixTruthBoundary} expectations=${pbMatrixExpectations}")
endif()

file(SHA256 "${pbMatrixOutput}" pbMatrixHashBefore)
execute_process(
    COMMAND "${PB_MATRIX_TOOL}" --output "${pbMatrixOutput}"
    RESULT_VARIABLE pbMatrixSecondResult
    OUTPUT_VARIABLE pbMatrixSecondStdout
    ERROR_VARIABLE pbMatrixSecondStderr)
if(NOT pbMatrixSecondResult EQUAL 2)
    message(FATAL_ERROR
        "existing-output retry did not fail as usage/evidence error: exit=${pbMatrixSecondResult} "
        "stdout=${pbMatrixSecondStdout} stderr=${pbMatrixSecondStderr}")
endif()
file(SHA256 "${pbMatrixOutput}" pbMatrixHashAfter)
if(NOT pbMatrixHashBefore STREQUAL pbMatrixHashAfter OR EXISTS "${pbMatrixPartial}")
    message(FATAL_ERROR "existing evidence changed or left a partial file")
endif()
