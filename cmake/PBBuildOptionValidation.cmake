function(PbValidateBuildOptions)
    if(PB_BUILD_PRESENTATION_GATE AND NOT (WIN32 AND BUILD_TESTING AND PB_BUILD_TESTS))
        message(FATAL_ERROR
            "PB_BUILD_PRESENTATION_GATE requires Windows, BUILD_TESTING=ON and PB_BUILD_TESTS=ON")
    endif()
    if(PB_BUILD_FUZZERS AND PB_BUILD_BENCHMARKS)
        message(FATAL_ERROR
            "PB_BUILD_FUZZERS and PB_BUILD_BENCHMARKS are mutually exclusive; configure separate build directories.")
    endif()
endfunction()
