add_executable(PBProtocolDescriptorResourceFuzz
    descriptor_resource_fuzz.cpp)

add_executable(PBProtocolBootstrapControlFuzz
    bootstrap_control_fuzz.cpp)

add_executable(PBProtocolOrphanResourceFuzz
    orphan_resource_fuzz.cpp)

# Build the same structured driver without libFuzzer's replacement main so
# post-checksum semantic reachability remains a deterministic CTest on every
# compiler backend, including Clang/libFuzzer builds.
add_executable(PBProtocolBootstrapControlStructuredSelfTest
    bootstrap_control_fuzz.cpp)

add_executable(PBCompressionZstdBoundaryFuzz
    compression_zstd_boundary_fuzz.cpp)

add_executable(PBOuterFecWirehairV2Fuzz
    wirehair_v2_wrapper_fuzz.cpp)

add_executable(PBOuterFecDirectRepeatFuzz
    direct_repeat_fuzz.cpp)

target_link_libraries(PBProtocolDescriptorResourceFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBProtocolBootstrapControlFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBProtocolOrphanResourceFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBProtocolBootstrapControlStructuredSelfTest
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBCompressionZstdBoundaryFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBCompression)

target_link_libraries(PBOuterFecWirehairV2Fuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBOuterFec)

target_link_libraries(PBOuterFecDirectRepeatFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBOuterFec)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(PBProtocolDescriptorResourceFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBProtocolBootstrapControlFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBProtocolOrphanResourceFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBCompressionZstdBoundaryFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBOuterFecWirehairV2Fuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBOuterFecDirectRepeatFuzz
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
    target_compile_options(PBOuterFec
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
    target_compile_options(PBProtocolBootstrapControlFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBProtocolBootstrapControlFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolOrphanResourceFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBProtocolOrphanResourceFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolBootstrapControlStructuredSelfTest
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_link_options(PBProtocolBootstrapControlStructuredSelfTest
        PRIVATE
            -fsanitize=address,undefined)
    target_compile_options(PBCompressionZstdBoundaryFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBCompressionZstdBoundaryFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBOuterFecWirehairV2Fuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBOuterFecWirehairV2Fuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBOuterFecDirectRepeatFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBOuterFecDirectRepeatFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
elseif(MSVC)
    # The MSVC fallback is a deterministic mutation runner. AddressSanitizer is
    # applied to each exercised PixelBridge library as well as all drivers so
    # library accesses are instrumented rather than only the outer executables.
    # C5072: ASan requires debug info for useful reports; /WX would otherwise
    # turn the warning into a hard error on non-RelWithDebInfo configurations.
    target_compile_options(PBProtocol PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBCompression PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBOuterFec PRIVATE /Zi /fsanitize=address)
    # The pinned vcpkg Wirehair archive is not ASan-instrumented and records
    # MSVC STL container annotations as disabled. Match that ABI in the two
    # PixelBridge targets that directly exchange STL-backed wrapper state;
    # ordinary ASan memory instrumentation remains enabled.
    target_compile_definitions(PBOuterFec
        PRIVATE
            _DISABLE_STRING_ANNOTATION
            _DISABLE_VECTOR_ANNOTATION)
    target_compile_definitions(PBOuterFecWirehairV2Fuzz
        PRIVATE
            _DISABLE_STRING_ANNOTATION
            _DISABLE_VECTOR_ANNOTATION)
    target_compile_definitions(PBOuterFecDirectRepeatFuzz
        PRIVATE
            _DISABLE_STRING_ANNOTATION
            _DISABLE_VECTOR_ANNOTATION)
    target_compile_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    # LNK4302: ASan asks for link-time debug info so reports can name symbols.
    target_link_options(PBProtocolDescriptorResourceFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolBootstrapControlFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolBootstrapControlFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolOrphanResourceFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolOrphanResourceFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolBootstrapControlStructuredSelfTest
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolBootstrapControlStructuredSelfTest
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
    target_compile_options(PBOuterFecWirehairV2Fuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBOuterFecWirehairV2Fuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBOuterFecDirectRepeatFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBOuterFecDirectRepeatFuzz
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
    add_custom_command(TARGET PBProtocolBootstrapControlFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBProtocolBootstrapControlFuzz>"
        VERBATIM)
    add_custom_command(TARGET PBProtocolOrphanResourceFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBProtocolOrphanResourceFuzz>"
        VERBATIM)
    add_custom_command(
        TARGET PBProtocolBootstrapControlStructuredSelfTest
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBProtocolBootstrapControlStructuredSelfTest>"
        VERBATIM)
    add_custom_command(TARGET PBCompressionZstdBoundaryFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBCompressionZstdBoundaryFuzz>"
        VERBATIM)
    add_custom_command(TARGET PBOuterFecWirehairV2Fuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBOuterFecWirehairV2Fuzz>"
        VERBATIM)
    add_custom_command(TARGET PBOuterFecDirectRepeatFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBOuterFecDirectRepeatFuzz>"
        VERBATIM)
    unset(pbMsvcAsanRuntime)
    unset(pbMsvcCompilerDirectory)
else()
    message(WARNING
        "PixelBridge fuzz targets will use deterministic mutation runners "
        "without sanitizer instrumentation on this compiler")
endif()

if(BUILD_TESTING)
    add_test(
        NAME PBProtocolBootstrapControlStructuredSelfTest
        COMMAND PBProtocolBootstrapControlStructuredSelfTest --self-test)
    set_tests_properties(
        PBProtocolBootstrapControlStructuredSelfTest
        PROPERTIES
            LABELS "fuzz;pbprotocol;bootstrap;control;fragment;receiver;structured"
            TIMEOUT 30)

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
            NAME PBProtocolBootstrapControlFuzzSmoke
            COMMAND PBProtocolBootstrapControlFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/bootstrap-control"
                -runs=2000)
        set_tests_properties(PBProtocolBootstrapControlFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;bootstrap;control;libfuzzer"
            TIMEOUT 60)
        add_test(
            NAME PBProtocolOrphanResourceFuzzSmoke
            COMMAND PBProtocolOrphanResourceFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/orphan-resource"
                -runs=2000)
        set_tests_properties(PBProtocolOrphanResourceFuzzSmoke PROPERTIES
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
        add_test(
            NAME PBOuterFecWirehairV2FuzzSmoke
            COMMAND PBOuterFecWirehairV2Fuzz
                -runs=1000)
        set_tests_properties(PBOuterFecWirehairV2FuzzSmoke PROPERTIES
            LABELS "fuzz;pbouterfec;libfuzzer"
            TIMEOUT 60)
        add_test(
            NAME PBOuterFecDirectRepeatFuzzSmoke
            COMMAND PBOuterFecDirectRepeatFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/direct-repeat"
                -runs=2000)
        set_tests_properties(PBOuterFecDirectRepeatFuzzSmoke PROPERTIES
            LABELS "fuzz;pbouterfec;direct-repeat;libfuzzer"
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
            NAME PBProtocolBootstrapControlFuzzSmoke
            COMMAND PBProtocolBootstrapControlFuzz
                2000
                5783258900934164481)
        set_tests_properties(PBProtocolBootstrapControlFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;bootstrap;control"
            TIMEOUT 60)
        add_test(
            NAME PBProtocolOrphanResourceFuzzSmoke
            COMMAND PBProtocolOrphanResourceFuzz
                2000
                7263948150273648113)
        set_tests_properties(PBProtocolOrphanResourceFuzzSmoke PROPERTIES
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
        add_test(
            NAME PBOuterFecWirehairV2FuzzSmoke
            COMMAND PBOuterFecWirehairV2Fuzz
                1000
                6289371488644456784)
        set_tests_properties(PBOuterFecWirehairV2FuzzSmoke PROPERTIES
            LABELS "fuzz;pbouterfec"
            TIMEOUT 60)
        add_test(
            NAME PBOuterFecDirectRepeatFuzzSmoke
            COMMAND PBOuterFecDirectRepeatFuzz
                2000
                4923072552113298010)
        set_tests_properties(PBOuterFecDirectRepeatFuzzSmoke PROPERTIES
            LABELS "fuzz;pbouterfec;direct-repeat"
            TIMEOUT 60)

        set(pbDirectRepeatCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/direct-repeat")
        foreach(pbDirectRepeatCorpusName IN ITEMS
                zero-one-byte
                short-tail
                malformed-shape
                noncanonical-padding
                duplicate-conflict)
            add_test(
                NAME "PBOuterFecDirectRepeatCorpus.${pbDirectRepeatCorpusName}"
                COMMAND PBOuterFecDirectRepeatFuzz
                    --input
                    "${pbDirectRepeatCorpusDirectory}/${pbDirectRepeatCorpusName}.bin")
            set_tests_properties(
                "PBOuterFecDirectRepeatCorpus.${pbDirectRepeatCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbouterfec;direct-repeat;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbDirectRepeatCorpusName)
        unset(pbDirectRepeatCorpusDirectory)

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

        set(pbOrphanResourceCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/orphan-resource")
        foreach(pbOrphanResourceCorpusName IN ITEMS
                valid-admit-sequence
                resume-record-valid
                resume-record-corrupt)
            add_test(
                NAME "PBProtocolOrphanResourceCorpus.${pbOrphanResourceCorpusName}"
                COMMAND PBProtocolOrphanResourceFuzz
                    --input
                    "${pbOrphanResourceCorpusDirectory}/${pbOrphanResourceCorpusName}.bin")
            set_tests_properties(
                "PBProtocolOrphanResourceCorpus.${pbOrphanResourceCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbprotocol;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbOrphanResourceCorpusName)
        unset(pbOrphanResourceCorpusDirectory)

        set(pbBootstrapControlCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/bootstrap-control")
        foreach(pbBootstrapControlCorpusName IN ITEMS
                valid-bootstrap
                valid-control-session
                bad-bootstrap-crc
                bad-control-crc
                valid-fragment-0
                valid-fragment-1
                valid-fragment-2
                semantic-bootstrap-version
                semantic-control-type
                semantic-fragment-flags
                valid-control-empty
                valid-control-maximum
                structured-out-of-order
                structured-duplicate
                structured-conflict
                structured-descriptor-conflict
                structured-extreme-metadata
                structured-quota-expiry)
            add_test(
                NAME "PBProtocolBootstrapControlCorpus.${pbBootstrapControlCorpusName}"
                COMMAND PBProtocolBootstrapControlFuzz
                    --input
                    "${pbBootstrapControlCorpusDirectory}/${pbBootstrapControlCorpusName}.bin")
            set_tests_properties(
                "PBProtocolBootstrapControlCorpus.${pbBootstrapControlCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbprotocol;bootstrap;control;fragment;receiver;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbBootstrapControlCorpusName)
        unset(pbBootstrapControlCorpusDirectory)

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
