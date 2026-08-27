foreach(pbWindows IN ITEMS ON OFF)
    foreach(pbTesting IN ITEMS ON OFF)
        foreach(pbTests IN ITEMS ON OFF)
            foreach(pbGate IN ITEMS ON OFF)
                execute_process(COMMAND "${CMAKE_COMMAND}"
                    "-DPB_MODULE_DIR=${PB_MODULE_DIR}"
                    "-DPB_PROBE_WINDOWS=${pbWindows}"
                    "-DBUILD_TESTING=${pbTesting}"
                    "-DPB_BUILD_TESTS=${pbTests}"
                    "-DPB_BUILD_WGC_GATE=${pbGate}"
                    -P "${PB_PROBE_SCRIPT}"
                    RESULT_VARIABLE pbResult OUTPUT_VARIABLE pbOutput ERROR_VARIABLE pbError)
                if(NOT pbGate OR (pbWindows AND pbTesting AND pbTests))
                    if(NOT pbResult EQUAL 0)
                        message(FATAL_ERROR "WGC build matrix rejected a valid combination: ${pbOutput}${pbError}")
                    endif()
                elseif(pbResult EQUAL 0 OR NOT pbError MATCHES "PB_BUILD_WGC_GATE requires Windows")
                    message(FATAL_ERROR "WGC build matrix accepted an invalid combination: ${pbOutput}${pbError}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
