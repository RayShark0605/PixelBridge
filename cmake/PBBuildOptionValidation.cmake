function(PbValidateBuildOptions)
    if(PB_BUILD_FUZZERS AND PB_BUILD_BENCHMARKS)
        message(FATAL_ERROR
            "PB_BUILD_FUZZERS and PB_BUILD_BENCHMARKS are mutually exclusive; configure separate build directories.")
    endif()
endfunction()
