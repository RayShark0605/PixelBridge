add_executable(PBProtocolDescriptorResourceFuzz
    descriptor_resource_fuzz.cpp)

target_link_libraries(PBProtocolDescriptorResourceFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(PBProtocolDescriptorResourceFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_options(PBProtocol
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
elseif(MSVC)
    # The MSVC fallback is a deterministic mutation runner. AddressSanitizer is
    # applied to PBProtocol itself as well as the driver so library accesses are
    # instrumented rather than only the outer executable.
    # C5072: ASan requires debug info for useful reports; /WX would otherwise
    # turn the warning into a hard error on non-RelWithDebInfo configurations.
    target_compile_options(PBProtocol PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /Zi
        /fsanitize=address)
    # LNK4302: ASan asks for link-time debug info so reports can name symbols.
    target_link_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)

    get_filename_component(pbMsvcCompilerDirectory
        "${CMAKE_CXX_COMPILER}"
        DIRECTORY)
    set(pbMsvcAsanRuntime
        "${pbMsvcCompilerDirectory}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${pbMsvcAsanRuntime}")
        message(FATAL_ERROR
            "MSVC AddressSanitizer runtime was not found: ${pbMsvcAsanRuntime}")
    endif()
    add_custom_command(TARGET PBProtocolDescriptorResourceFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBProtocolDescriptorResourceFuzz>"
        VERBATIM)
    unset(pbMsvcAsanRuntime)
    unset(pbMsvcCompilerDirectory)
else()
    message(WARNING
        "PBProtocolDescriptorResourceFuzz will use the deterministic mutation "
        "runner without sanitizer instrumentation on this compiler")
endif()

if(BUILD_TESTING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_test(
            NAME PBProtocolDescriptorResourceFuzzSmoke
            COMMAND PBProtocolDescriptorResourceFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/descriptor-resource"
                -runs=2000)
        set_tests_properties(PBProtocolDescriptorResourceFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;libfuzzer"
            TIMEOUT 60)
    else()
        add_test(
            NAME PBProtocolDescriptorResourceFuzzSmoke
            COMMAND PBProtocolDescriptorResourceFuzz
                2000
                13464654573299691533)
        set_tests_properties(PBProtocolDescriptorResourceFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol"
            TIMEOUT 60)

        set(pbDescriptorCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/descriptor-resource")
        foreach(pbDescriptorCorpusName IN ITEMS
                valid-session
                valid-direct-segment
                valid-final-manifest
                overflow-direct-segment)
            add_test(
                NAME "PBProtocolDescriptorCorpus.${pbDescriptorCorpusName}"
                COMMAND PBProtocolDescriptorResourceFuzz
                    --input
                    "${pbDescriptorCorpusDirectory}/${pbDescriptorCorpusName}.bin")
            set_tests_properties(
                "PBProtocolDescriptorCorpus.${pbDescriptorCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbprotocol;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbDescriptorCorpusName)
        unset(pbDescriptorCorpusDirectory)
    endif()
endif()
