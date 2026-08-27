if(NOT DEFINED PB_HOST_PROBE OR PB_HOST_PROBE STREQUAL "")
    message(FATAL_ERROR "PB_HOST_PROBE is required")
endif()
execute_process(COMMAND "${PB_HOST_PROBE}" --select-region
    RESULT_VARIABLE pbResult OUTPUT_VARIABLE pbStdout ERROR_VARIABLE pbStderr TIMEOUT 10)
string(REPLACE "\r\n" "\n" pbStderr "${pbStderr}")
if(NOT pbResult STREQUAL "1" OR NOT pbStdout STREQUAL "" OR
   NOT pbStderr STREQUAL "Screen region: dpi-awareness-required; stage=1; native=0\n")
    message(FATAL_ERROR "Decoder runtime failure contract: exit=${pbResult}; stdout=${pbStdout}; stderr=${pbStderr}")
endif()
