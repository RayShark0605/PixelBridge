# This driver uses the same production PBModulation instrumentation configured
# by CMakeLists.txt. Its own main is deterministic on MSVC; do not label that
# backend as coverage-guided libFuzzer or UBSan.
add_executable(PBModulationLocalDesktopBootstrapFuzz local_desktop_bootstrap_fuzz.cpp)
target_link_libraries(PBModulationLocalDesktopBootstrapFuzz PRIVATE PB::CompilerSettings PB::PBModulation)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(PBModulationLocalDesktopBootstrapFuzz PRIVATE PB_USE_LIBFUZZER=1)
    target_compile_options(PBModulationLocalDesktopBootstrapFuzz PRIVATE -fno-omit-frame-pointer -fno-sanitize-recover=undefined -fsanitize=fuzzer,address,undefined)
    target_link_options(PBModulationLocalDesktopBootstrapFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
elseif(MSVC)
    target_compile_definitions(PBModulationLocalDesktopBootstrapFuzz PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    target_compile_options(PBModulationLocalDesktopBootstrapFuzz PRIVATE /Zi /fsanitize=address)
    target_link_options(PBModulationLocalDesktopBootstrapFuzz PRIVATE /DEBUG /INCREMENTAL:NO)
    get_filename_component(pbLocalFuzzCompilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(pbLocalFuzzAsanRuntime "${pbLocalFuzzCompilerDirectory}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${pbLocalFuzzAsanRuntime}")
        message(FATAL_ERROR "MSVC ASan runtime unavailable: ${pbLocalFuzzAsanRuntime}")
    endif()
    add_custom_command(TARGET PBModulationLocalDesktopBootstrapFuzz POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${pbLocalFuzzAsanRuntime}" "$<TARGET_FILE_DIR:PBModulationLocalDesktopBootstrapFuzz>" VERBATIM)
    unset(pbLocalFuzzCompilerDirectory)
    unset(pbLocalFuzzAsanRuntime)
endif()

if(BUILD_TESTING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # libFuzzer writes its discoveries. Never mutate the independent seeds
        # committed under source control while running a corpus smoke.
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/local-desktop-mutation-corpus")
        file(GLOB pbLocalFuzzSeeds "${CMAKE_CURRENT_SOURCE_DIR}/corpus/local-desktop-bootstrap/*.bin")
        file(COPY ${pbLocalFuzzSeeds} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/local-desktop-mutation-corpus")
        add_test(NAME PBModulationLocalDesktopBootstrapFuzzSmoke
            COMMAND PBModulationLocalDesktopBootstrapFuzz "${CMAKE_CURRENT_BINARY_DIR}/local-desktop-mutation-corpus" -runs=256 -max_len=4096)
        set_tests_properties(PBModulationLocalDesktopBootstrapFuzzSmoke PROPERTIES
            LABELS "fuzz;local-desktop;parser-harness;libfuzzer" TIMEOUT 180)
        unset(pbLocalFuzzSeeds)
    else()
        add_test(NAME PBModulationLocalDesktopBootstrapFuzzSmoke
            COMMAND PBModulationLocalDesktopBootstrapFuzz 64 5783543126721007665)
        set_tests_properties(PBModulationLocalDesktopBootstrapFuzzSmoke PROPERTIES
            LABELS "fuzz;local-desktop;parser-harness" TIMEOUT 180
            PASS_REGULAR_EXPRESSION "LOCAL_DESKTOP_FUZZ_COMPLETED")
        foreach(pbLocalCorpus IN ITEMS rs-correction16 rs-long-input rs-short-input rs-short-output
                view-coordinate-nonfinite view-fp16-nan view-overflow view-padded-gray
                visual-blend50 visual-clean visual-correction16 visual-foreign-copy visual-torn-horizontal visual-torn-vertical)
            add_test(NAME "PBModulationLocalDesktopCorpus.${pbLocalCorpus}"
                COMMAND PBModulationLocalDesktopBootstrapFuzz --input "${CMAKE_CURRENT_SOURCE_DIR}/corpus/local-desktop-bootstrap/${pbLocalCorpus}.bin")
            set_tests_properties("PBModulationLocalDesktopCorpus.${pbLocalCorpus}" PROPERTIES
                LABELS "fuzz;local-desktop;parser-harness;corpus" TIMEOUT 30
                PASS_REGULAR_EXPRESSION "CORPUS_REPLAY_VALIDATED")
        endforeach()
        unset(pbLocalCorpus)
    endif()
endif()
