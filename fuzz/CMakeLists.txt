add_executable(PBProtocolDescriptorResourceFuzz
    descriptor_resource_fuzz.cpp)

add_executable(PBCompressionZstdBoundaryFuzz
    compression_zstd_boundary_fuzz.cpp)

target_link_libraries(PBProtocolDescriptorResourceFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBCompressionZstdBoundaryFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBCompression)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(PBProtocolDescriptorResourceFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBCompressionZstdBoundaryFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_options(PBProtocol
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBCompression
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
    target_compile_options(PBCompressionZstdBoundaryFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBCompressionZstdBoundaryFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
elseif(MSVC)
    # The MSVC fallback is a deterministic mutation runner. AddressSanitizer is
    # applied to PBProtocol and PBCompression as well as both drivers so library
    # accesses are instrumented rather than only the outer executables.
    # C5072: ASan requires debug info for useful reports; /WX would otherwise
    # turn the warning into a hard error on non-RelWithDebInfo configurations.
    target_compile_options(PBProtocol PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBCompression PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    # LNK4302: ASan asks for link-time debug info so reports can name symbols.
    target_link_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBCompressionZstdBoundaryFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBCompressionZstdBoundaryFuzz
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
    add_custom_command(TARGET PBCompressionZstdBoundaryFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBCompressionZstdBoundaryFuzz>"
        VERBATIM)
    unset(pbMsvcAsanRuntime)
    unset(pbMsvcCompilerDirectory)
else()
    message(WARNING
        "PixelBridge fuzz targets will use deterministic mutation runners "
        "without sanitizer instrumentation on this compiler")
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
        add_test(
            NAME PBCompressionZstdBoundaryFuzzSmoke
            COMMAND PBCompressionZstdBoundaryFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/compression-zstd"
                -runs=2000)
        set_tests_properties(PBCompressionZstdBoundaryFuzzSmoke PROPERTIES
            LABELS "fuzz;pbcompression;libfuzzer"
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
        add_test(
            NAME PBCompressionZstdBoundaryFuzzSmoke
            COMMAND PBCompressionZstdBoundaryFuzz
                2000
                13856851484949778996)
        set_tests_properties(PBCompressionZstdBoundaryFuzzSmoke PROPERTIES
            LABELS "fuzz;pbcompression"
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

        set(pbCompressionCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/compression-zstd")
        foreach(pbCompressionCorpusName IN ITEMS
                empty-output
                known-answer
                unknown-fcs
                wide-window
                truncated-header
                truncated-block
                truncated-checksum
                checksum-corruption
                trailing-byte
                double-frame
                skippable-frame
                invalid-magic
                quota-limit-minus-one
                quota-limit
                quota-limit-plus-one)
            add_test(
                NAME "PBCompressionCorpus.${pbCompressionCorpusName}"
                COMMAND PBCompressionZstdBoundaryFuzz
                    --input
                    "${pbCompressionCorpusDirectory}/${pbCompressionCorpusName}.bin")
            set_tests_properties(
                "PBCompressionCorpus.${pbCompressionCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbcompression;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbCompressionCorpusName)
        unset(pbCompressionCorpusDirectory)
    endif()
endif()
