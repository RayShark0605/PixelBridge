if(NOT DEFINED PB_MODULE_DIR)
    message(FATAL_ERROR "PB_MODULE_DIR is required")
endif()

if(NOT DEFINED PB_PROBE_SCRIPT)
    message(FATAL_ERROR "PB_PROBE_SCRIPT is required")
endif()

function(RunBuildOptionProbe fuzzEnabled benchmarkEnabled expectSuccess)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPB_MODULE_DIR=${PB_MODULE_DIR}"
            "-DPB_BUILD_FUZZERS=${fuzzEnabled}"
            "-DPB_BUILD_BENCHMARKS=${benchmarkEnabled}"
            -P "${PB_PROBE_SCRIPT}"
        RESULT_VARIABLE probeResult
        OUTPUT_VARIABLE probeStdout
        ERROR_VARIABLE probeStderr)

    if(expectSuccess)
        if(NOT probeResult EQUAL 0)
            message(FATAL_ERROR
                "Build option probe unexpectedly failed for fuzz=${fuzzEnabled}, "
                "benchmark=${benchmarkEnabled}:\n${probeStdout}\n${probeStderr}")
        endif()
        return()
    endif()

    if(probeResult EQUAL 0)
        message(FATAL_ERROR
            "Build option probe unexpectedly accepted fuzz=${fuzzEnabled}, "
            "benchmark=${benchmarkEnabled}")
    endif()

    string(CONCAT probeOutput "${probeStdout}" "${probeStderr}")
    if(NOT probeOutput MATCHES "mutually exclusive")
        message(FATAL_ERROR
            "Build option rejection omitted the required diagnostic:\n${probeOutput}")
    endif()
endfunction()

RunBuildOptionProbe(ON OFF TRUE)
RunBuildOptionProbe(OFF ON TRUE)
RunBuildOptionProbe(ON ON FALSE)
