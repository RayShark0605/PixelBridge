# Compare switch names as literal strings, not as host-platform variables.
cmake_policy(SET CMP0054 NEW)

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

# The desktop-levels native matrix also requires the actual applications and
# the shared offline evaluator, not a substitute test-only decoder.
foreach(pbProbeMask RANGE 0 63)
    set(pbSwitches WIN32 BUILD_TESTING PB_BUILD_TESTS PB_BUILD_APPS PB_BUILD_TOOLS PB_BUILD_DESKTOP_LEVELS_GATE)
    set(pbArguments "-DPB_MODULE_DIR=${PB_MODULE_DIR}")
    set(pbIndex 0)
    foreach(pbSwitch IN LISTS pbSwitches)
        math(EXPR pbValue "(${pbProbeMask} >> ${pbIndex}) & 1")
        if(pbSwitch STREQUAL "WIN32")
            list(APPEND pbArguments "-DPB_PROBE_WINDOWS=${pbValue}")
        else()
            list(APPEND pbArguments "-D${pbSwitch}=${pbValue}")
        endif()
        math(EXPR pbIndex "${pbIndex} + 1")
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${pbArguments} -P "${PB_PROBE_SCRIPT}"
        RESULT_VARIABLE pbProbeResult OUTPUT_VARIABLE pbProbeOutput ERROR_VARIABLE pbProbeError)
    if(pbProbeMask LESS 32 OR pbProbeMask EQUAL 63)
        if(NOT pbProbeResult EQUAL 0)
            message(FATAL_ERROR "DesktopLevels rejected valid option mask ${pbProbeMask}: ${pbProbeOutput}${pbProbeError}")
        endif()
    elseif(pbProbeResult EQUAL 0 OR NOT pbProbeError MATCHES "PB_BUILD_DESKTOP_LEVELS_GATE requires Windows")
        message(FATAL_ERROR "DesktopLevels accepted invalid option mask ${pbProbeMask}: ${pbProbeOutput}${pbProbeError}")
    endif()
endforeach()

foreach(pbNativeGate IN ITEMS PB_BUILD_WGC_GATE PB_BUILD_DXGI_GATE PB_BUILD_LOCAL_DESKTOP_GATE)
    foreach(pbProbeWindows IN ITEMS ON OFF)
        foreach(pbProbeTesting IN ITEMS ON OFF)
            foreach(pbProbeTests IN ITEMS ON OFF)
                foreach(pbProbeGate IN ITEMS ON OFF)
                    execute_process(
                        COMMAND "${CMAKE_COMMAND}"
                            "-DPB_MODULE_DIR=${PB_MODULE_DIR}"
                            "-DPB_PROBE_WINDOWS=${pbProbeWindows}"
                            "-DBUILD_TESTING=${pbProbeTesting}"
                            "-DPB_BUILD_TESTS=${pbProbeTests}"
                            "-D${pbNativeGate}=${pbProbeGate}"
                            -P "${PB_PROBE_SCRIPT}"
                        RESULT_VARIABLE pbProbeResult OUTPUT_VARIABLE pbProbeOutput ERROR_VARIABLE pbProbeError)
                    if(NOT pbProbeGate OR (pbProbeWindows AND pbProbeTesting AND pbProbeTests))
                        if(NOT pbProbeResult EQUAL 0)
                            message(FATAL_ERROR "${pbNativeGate} rejected a valid combination: ${pbProbeOutput}${pbProbeError}")
                        endif()
                    elseif(pbProbeResult EQUAL 0 OR NOT pbProbeError MATCHES "${pbNativeGate} requires Windows")
                        message(FATAL_ERROR "${pbNativeGate} accepted an invalid combination or lost its diagnostic: ${pbProbeOutput}${pbProbeError}")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endforeach()

# Model the platform/test/gate switches in the same validator called by the
# real root configure. Opting out never creates an interactive test target.
foreach(pbProbeWindows IN ITEMS ON OFF)
    foreach(pbProbeTesting IN ITEMS ON OFF)
        foreach(pbProbeTests IN ITEMS ON OFF)
            foreach(pbProbeGate IN ITEMS ON OFF)
                execute_process(
                    COMMAND "${CMAKE_COMMAND}"
                        "-DPB_MODULE_DIR=${PB_MODULE_DIR}"
                        "-DPB_PROBE_WINDOWS=${pbProbeWindows}"
                        "-DBUILD_TESTING=${pbProbeTesting}"
                        "-DPB_BUILD_TESTS=${pbProbeTests}"
                        "-DPB_BUILD_PRESENTATION_GATE=${pbProbeGate}"
                        -P "${PB_PROBE_SCRIPT}"
                    RESULT_VARIABLE pbProbeResult
                    OUTPUT_VARIABLE pbProbeOutput
                    ERROR_VARIABLE pbProbeError)
                if(NOT pbProbeGate OR (pbProbeWindows AND pbProbeTesting AND pbProbeTests))
                    if(NOT pbProbeResult EQUAL 0)
                        message(FATAL_ERROR "Presentation build matrix rejected a valid combination: ${pbProbeOutput}${pbProbeError}")
                    endif()
                elseif(pbProbeResult EQUAL 0 OR NOT pbProbeError MATCHES "PB_BUILD_PRESENTATION_GATE requires Windows")
                    message(FATAL_ERROR "Presentation build matrix accepted an invalid combination or lost its diagnostic: ${pbProbeOutput}${pbProbeError}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()

foreach(pbProbeWindows IN ITEMS ON OFF)
    foreach(pbProbeTesting IN ITEMS ON OFF)
        foreach(pbProbeTests IN ITEMS ON OFF)
            foreach(pbProbeGate IN ITEMS ON OFF)
                execute_process(
                    COMMAND "${CMAKE_COMMAND}"
                        "-DPB_MODULE_DIR=${PB_MODULE_DIR}"
                        "-DPB_PROBE_WINDOWS=${pbProbeWindows}"
                        "-DBUILD_TESTING=${pbProbeTesting}"
                        "-DPB_BUILD_TESTS=${pbProbeTests}"
                        "-DPB_BUILD_SCREEN_REGION_GATE=${pbProbeGate}"
                        -P "${PB_PROBE_SCRIPT}"
                    RESULT_VARIABLE pbProbeResult
                    OUTPUT_VARIABLE pbProbeOutput
                    ERROR_VARIABLE pbProbeError)
                if(NOT pbProbeGate OR (pbProbeWindows AND pbProbeTesting AND pbProbeTests))
                    if(NOT pbProbeResult EQUAL 0)
                        message(FATAL_ERROR "Screen-region build matrix rejected a valid combination: ${pbProbeOutput}${pbProbeError}")
                    endif()
                elseif(pbProbeResult EQUAL 0 OR NOT pbProbeError MATCHES "PB_BUILD_SCREEN_REGION_GATE requires Windows")
                    message(FATAL_ERROR "Screen-region build matrix accepted an invalid combination or lost its diagnostic: ${pbProbeOutput}${pbProbeError}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
