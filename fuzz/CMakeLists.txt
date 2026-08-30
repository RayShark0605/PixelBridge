add_executable(PBProtocolDescriptorResourceFuzz
    descriptor_resource_fuzz.cpp)

add_executable(PBProtocolBootstrapControlFuzz
    bootstrap_control_fuzz.cpp)

add_executable(PBProtocolOrphanResourceFuzz
    orphan_resource_fuzz.cpp)

# Build the same structured driver without libFuzzer's replacement main so the
# resume.state reference-model checks remain a deterministic CTest on every
# compiler backend, including Clang/libFuzzer builds.
add_executable(PBProtocolResumeStateFuzz
    resume_state_fuzz.cpp)

add_executable(PBProtocolResumeStateStructuredSelfTest
    resume_state_fuzz.cpp)

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

add_executable(PBModulationReferenceRasterFuzz
    reference_raster_fuzz.cpp)

include(local_desktop_bootstrap.cmake)

add_executable(PBProtocolTransportFuzz
    transport_fuzz.cpp)

add_executable(PBProtocolTransportStructuredSelfTest
    transport_fuzz.cpp)

add_executable(PBInterleaveReferenceFuzz
    interleave_reference_fuzz.cpp)

add_executable(PBInnerFecCodewordFuzz
    inner_fec_codeword_fuzz.cpp)

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

target_link_libraries(PBProtocolResumeStateFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBProtocolResumeStateStructuredSelfTest
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

target_link_libraries(PBModulationReferenceRasterFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBModulation)

target_link_libraries(PBProtocolTransportFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBProtocolTransportStructuredSelfTest
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBInterleaveReferenceFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBInterleave)

target_link_libraries(PBInnerFecCodewordFuzz
    PRIVATE
        PB::CompilerSettings
        PB::PBInnerFec
        PB::PBProtocol)

add_custom_target(PBParserFuzzHarness
    DEPENDS
        PBProtocolBootstrapControlFuzz
        PBProtocolDescriptorResourceFuzz
        PBOuterFecWirehairV2Fuzz
        PBModulationReferenceRasterFuzz
        PBModulationLocalDesktopBootstrapFuzz
        PBProtocolTransportFuzz
        PBProtocolTransportStructuredSelfTest
        PBInterleaveReferenceFuzz
        PBInnerFecCodewordFuzz
        PBProtocolResumeStateFuzz
        PBProtocolResumeStateStructuredSelfTest)

include(desktop_levels.cmake)

target_include_directories(PBOuterFecWirehairV2Fuzz
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBOuterFec/src")

target_include_directories(PBOuterFecDirectRepeatFuzz
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/PBOuterFec/src")

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
    target_compile_definitions(PBProtocolResumeStateFuzz
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
    target_compile_definitions(PBModulationReferenceRasterFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBProtocolTransportFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBInterleaveReferenceFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_definitions(PBInnerFecCodewordFuzz
        PRIVATE
            PB_USE_LIBFUZZER=1)
    target_compile_options(PBProtocol
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer-no-link
            -fno-sanitize-recover=undefined
            -fsanitize=address,undefined)
    target_compile_options(PBRemoteVisualSimulator
        PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined
            -fsanitize=address,undefined)
    target_compile_options(PBCompression
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBOuterFec
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBModulation
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer-no-link
            -fno-sanitize-recover=undefined
            -fsanitize=address,undefined)
    target_compile_options(PBInterleave
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBInnerFec
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_compile_options(PBReceiver
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    foreach(pbPresentationTarget IN ITEMS PBPresentTiming PBRenderD3D PBScreenRegion PBCaptureNormalize PBScreenCaptureWgc PBScreenCaptureDxgi
            PBDemodD3D11 PBTelemetry PBRealCaptureReplay PBStorage PBApplication PixelBridgeEncoder PixelBridgeDecoder)
        if(TARGET ${pbPresentationTarget})
            target_compile_options(${pbPresentationTarget} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
            if(pbPresentationTarget STREQUAL "PixelBridgeEncoder" OR pbPresentationTarget STREQUAL "PixelBridgeDecoder")
                target_link_options(${pbPresentationTarget} PRIVATE -fsanitize=address,undefined)
            endif()
        endif()
    endforeach()
    unset(pbPresentationTarget)
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
    target_compile_options(PBModulationReferenceRasterFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBModulationReferenceRasterFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolTransportFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBProtocolTransportFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolTransportStructuredSelfTest
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_link_options(PBProtocolTransportStructuredSelfTest
        PRIVATE
            -fsanitize=address,undefined)
    target_compile_options(PBInterleaveReferenceFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBInterleaveReferenceFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBInnerFecCodewordFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBInnerFecCodewordFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolResumeStateFuzz
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=fuzzer,address,undefined)
    target_link_options(PBProtocolResumeStateFuzz
        PRIVATE
            -fsanitize=fuzzer,address,undefined)
    target_compile_options(PBProtocolResumeStateStructuredSelfTest
        PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    target_link_options(PBProtocolResumeStateStructuredSelfTest
        PRIVATE
            -fsanitize=address,undefined)
elseif(MSVC)
    # The MSVC fallback is a deterministic mutation runner. AddressSanitizer is
    # applied to each exercised PixelBridge library as well as all drivers so
    # library accesses are instrumented rather than only the outer executables.
    # C5072: ASan requires debug info for useful reports; /WX would otherwise
    # turn the warning into a hard error on non-RelWithDebInfo configurations.
    target_compile_options(PBProtocol PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBRemoteVisualSimulator PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBCompression PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBOuterFec PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBModulation PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBInterleave PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBInnerFec PRIVATE /Zi /fsanitize=address)
    target_compile_options(PBReceiver PRIVATE /Zi /fsanitize=address)
    foreach(pbPresentationTarget IN ITEMS PBPresentTiming PBRenderD3D PBScreenRegion PBCaptureNormalize PBScreenCaptureWgc PBScreenCaptureDxgi
            PBDemodD3D11 PBTelemetry PBRealCaptureReplay PBStorage PBApplication PixelBridgeEncoder PixelBridgeDecoder)
        if(TARGET ${pbPresentationTarget})
            target_compile_options(${pbPresentationTarget} PRIVATE /Zi /fsanitize=address)
            target_compile_definitions(${pbPresentationTarget} PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
            if(pbPresentationTarget STREQUAL "PixelBridgeEncoder" OR pbPresentationTarget STREQUAL "PixelBridgeDecoder")
                target_link_options(${pbPresentationTarget} PRIVATE /DEBUG /INCREMENTAL:NO)
            endif()
        endif()
    endforeach()
    unset(pbPresentationTarget)
    # The pinned vcpkg Wirehair archive records MSVC STL container annotations
    # as disabled. A single executable must use one annotation setting across
    # every linked C++ object (LNK2038). BoundSegmentDescriptor now makes the
    # Outer-FEC fuzzers pull PBProtocol objects into the final link, so keep the
    # entire fuzz-only C++ graph consistent. Ordinary ASan memory
    # instrumentation remains enabled; only STL container annotations are
    # disabled in this dedicated build tree.
    foreach(pbMsvcAsanTarget IN ITEMS
            PBProtocol
            PBRemoteVisualSimulator
            PBCompression
            PBOuterFec
            PBModulation
            PBInterleave
            PBInnerFec
            PBReceiver
            PBModulationReferenceRasterFuzz
            PBProtocolDescriptorResourceFuzz
            PBProtocolBootstrapControlFuzz
            PBProtocolOrphanResourceFuzz
            PBProtocolBootstrapControlStructuredSelfTest
            PBCompressionZstdBoundaryFuzz
            PBOuterFecWirehairV2Fuzz
            PBOuterFecDirectRepeatFuzz
            PBProtocolTransportFuzz
            PBProtocolTransportStructuredSelfTest
            PBInterleaveReferenceFuzz
            PBInnerFecCodewordFuzz
            PBProtocolResumeStateFuzz
            PBProtocolResumeStateStructuredSelfTest)
        target_compile_definitions(${pbMsvcAsanTarget}
            PRIVATE
                _DISABLE_STRING_ANNOTATION
                _DISABLE_VECTOR_ANNOTATION)
    endforeach()
    unset(pbMsvcAsanTarget)
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
    target_compile_options(PBModulationReferenceRasterFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBModulationReferenceRasterFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolTransportFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolTransportFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolTransportStructuredSelfTest
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolTransportStructuredSelfTest
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBInterleaveReferenceFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBInterleaveReferenceFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBInnerFecCodewordFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBInnerFecCodewordFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolResumeStateFuzz
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolResumeStateFuzz
        PRIVATE
            /DEBUG
            /INCREMENTAL:NO)
    target_compile_options(PBProtocolResumeStateStructuredSelfTest
        PRIVATE
            /Zi
            /fsanitize=address)
    target_link_options(PBProtocolResumeStateStructuredSelfTest
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
    add_custom_command(TARGET PBModulationReferenceRasterFuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pbMsvcAsanRuntime}"
            "$<TARGET_FILE_DIR:PBModulationReferenceRasterFuzz>"
        VERBATIM)
    foreach(pbMsvcAsanExecutable IN ITEMS
            PBProtocolTransportFuzz
            PBProtocolTransportStructuredSelfTest
            PBInterleaveReferenceFuzz
            PBInnerFecCodewordFuzz
            PBProtocolResumeStateFuzz
            PBProtocolResumeStateStructuredSelfTest)
        add_custom_command(TARGET ${pbMsvcAsanExecutable} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${pbMsvcAsanRuntime}"
                "$<TARGET_FILE_DIR:${pbMsvcAsanExecutable}>"
            VERBATIM)
    endforeach()
    unset(pbMsvcAsanExecutable)
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
    add_test(
        NAME PBProtocolTransportStructuredSelfTest
        COMMAND PBProtocolTransportStructuredSelfTest --self-test)
    set_tests_properties(
        PBProtocolTransportStructuredSelfTest
        PROPERTIES
            LABELS "fuzz;pbprotocol;transport;structured"
            TIMEOUT 30)

    add_test(
        NAME PBProtocolResumeStateStructuredSelfTest
        COMMAND PBProtocolResumeStateStructuredSelfTest --self-test)
    set_tests_properties(
        PBProtocolResumeStateStructuredSelfTest
        PROPERTIES
            LABELS "fuzz;pbprotocol;resume-state;structured"
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
        add_test(
            NAME PBModulationReferenceRasterFuzzSmoke
            COMMAND PBModulationReferenceRasterFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/reference-raster"
                -runs=2000)
        set_tests_properties(PBModulationReferenceRasterFuzzSmoke PROPERTIES
            LABELS "fuzz;pbmodulation;reference-raster;libfuzzer"
            TIMEOUT 60)
        add_test(
            NAME PBProtocolTransportFuzzSmoke
            COMMAND PBProtocolTransportFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/transport"
                -runs=2000)
        set_tests_properties(PBProtocolTransportFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;transport;libfuzzer"
            TIMEOUT 60)
        add_test(
            NAME PBInterleaveReferenceFuzzSmoke
            COMMAND PBInterleaveReferenceFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/interleave"
                -runs=2000)
        set_tests_properties(PBInterleaveReferenceFuzzSmoke PROPERTIES
            LABELS "fuzz;pbinterleave;libfuzzer"
            TIMEOUT 60)
        add_test(
            NAME PBInnerFecCodewordFuzzSmoke
            COMMAND PBInnerFecCodewordFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/ldpc"
                -runs=2000)
        set_tests_properties(PBInnerFecCodewordFuzzSmoke PROPERTIES
            LABELS "fuzz;pbinnerfec;ldpc;libfuzzer"
            TIMEOUT 120)
        add_test(
            NAME PBProtocolResumeStateFuzzSmoke
            COMMAND PBProtocolResumeStateFuzz
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/resume-state"
                -runs=2000)
        set_tests_properties(PBProtocolResumeStateFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;resume-state;libfuzzer"
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
        # Full 8294400-byte frames are synthesized in memory by the seeded
        # mutation loop; the corpus keeps small replay seeds only (repo-size
        # decision, consistent with the other corpora).
        add_test(
            NAME PBModulationReferenceRasterFuzzSmoke
            COMMAND PBModulationReferenceRasterFuzz
                1000
                4761939155204273851)
        set_tests_properties(PBModulationReferenceRasterFuzzSmoke PROPERTIES
            LABELS "fuzz;pbmodulation;reference-raster"
            TIMEOUT 600)
        add_test(
            NAME PBProtocolTransportFuzzSmoke
            COMMAND PBProtocolTransportFuzz
                100000
                14627333968688430831)
        set_tests_properties(PBProtocolTransportFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;transport;deterministic"
            TIMEOUT 120)
        add_test(
            NAME PBInterleaveReferenceFuzzSmoke
            COMMAND PBInterleaveReferenceFuzz
                1000
                1161981756646125696)
        set_tests_properties(PBInterleaveReferenceFuzzSmoke PROPERTIES
            LABELS "fuzz;pbinterleave;deterministic"
            TIMEOUT 120)
        add_test(
            NAME PBInnerFecCodewordFuzzSmoke
            COMMAND PBInnerFecCodewordFuzz
                1000
                11068027678940948070)
        set_tests_properties(PBInnerFecCodewordFuzzSmoke PROPERTIES
            LABELS "fuzz;pbinnerfec;ldpc;deterministic"
            TIMEOUT 120)
        # Deterministic resume.state driver: structured reference-model checks
        # plus a fixed-seed mutation sweep (report-grade runs use more).
        add_test(
            NAME PBProtocolResumeStateFuzzSmoke
            COMMAND PBProtocolResumeStateFuzz
                2000
                14059238575124963127)
        set_tests_properties(PBProtocolResumeStateFuzzSmoke PROPERTIES
            LABELS "fuzz;pbprotocol;resume-state"
            TIMEOUT 60)

        foreach(pbCorpusCategory IN ITEMS transport interleave ldpc)
            file(GLOB pbFocusedCorpusFiles CONFIGURE_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${pbCorpusCategory}/*.bin")
            foreach(pbFocusedCorpusFile IN LISTS pbFocusedCorpusFiles)
                get_filename_component(pbFocusedCorpusName "${pbFocusedCorpusFile}" NAME_WE)
                if(pbCorpusCategory STREQUAL "transport")
                    set(pbFocusedCorpusTarget PBProtocolTransportFuzz)
                elseif(pbCorpusCategory STREQUAL "interleave")
                    set(pbFocusedCorpusTarget PBInterleaveReferenceFuzz)
                else()
                    set(pbFocusedCorpusTarget PBInnerFecCodewordFuzz)
                endif()
                add_test(
                    NAME "${pbFocusedCorpusTarget}Corpus.${pbFocusedCorpusName}"
                    COMMAND ${pbFocusedCorpusTarget} --input "${pbFocusedCorpusFile}")
                set_tests_properties(
                    "${pbFocusedCorpusTarget}Corpus.${pbFocusedCorpusName}"
                    PROPERTIES
                        LABELS "fuzz;parser-harness;${pbCorpusCategory};corpus"
                        TIMEOUT 30)
            endforeach()
        endforeach()
        unset(pbFocusedCorpusTarget)
        unset(pbFocusedCorpusName)
        unset(pbFocusedCorpusFile)
        unset(pbFocusedCorpusFiles)
        unset(pbCorpusCategory)

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
                full-count-duplicate-conflict
                full-byte-duplicate-conflict
                declared-length-conflict
                noncanonical-padding
                oversized-orphan
                resume-record-valid
                resume-record-corrupt
                resume-record-exact-limit)
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

        set(pbResumeStateCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/resume-state")
        foreach(pbResumeStateCorpusName IN ITEMS
                valid-single-completed
                valid-multi-record
                malformed-bad-magic
                malformed-version
                malformed-reserved
                malformed-crc-payload
                malformed-body-trailing
                malformed-unknown-tag
                quota-document-over-budget
                semantic-cross-type-conflict
                semantic-wirehair-id-conflict
                torn-mid-envelope
                torn-short-header)
            add_test(
                NAME "PBProtocolResumeStateCorpus.${pbResumeStateCorpusName}"
                COMMAND PBProtocolResumeStateFuzz
                    --input
                    "${pbResumeStateCorpusDirectory}/${pbResumeStateCorpusName}.bin")
            set_tests_properties(
                "PBProtocolResumeStateCorpus.${pbResumeStateCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbprotocol;resume-state;corpus"
                    TIMEOUT 10)
        endforeach()
        unset(pbResumeStateCorpusName)
        unset(pbResumeStateCorpusDirectory)

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

        set(pbReferenceRasterCorpusDirectory
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/reference-raster")
        foreach(pbReferenceRasterCorpusName IN ITEMS
                valid-manifest
                corrupted-manifest
                valid-raw-small
                valid-png-small
                truncated-png
                invalid-raw-magic)
            add_test(
                NAME "PBModulationReferenceRasterCorpus.${pbReferenceRasterCorpusName}"
                COMMAND PBModulationReferenceRasterFuzz
                    --input
                    "${pbReferenceRasterCorpusDirectory}/${pbReferenceRasterCorpusName}.bin")
            set_tests_properties(
                "PBModulationReferenceRasterCorpus.${pbReferenceRasterCorpusName}"
                PROPERTIES
                    LABELS "fuzz;pbmodulation;reference-raster;corpus"
                    TIMEOUT 30)
        endforeach()
        unset(pbReferenceRasterCorpusName)
        unset(pbReferenceRasterCorpusDirectory)

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

    get_property(pbRegisteredFuzzTests DIRECTORY PROPERTY TESTS)
    foreach(pbRegisteredFuzzTest IN LISTS pbRegisteredFuzzTests)
        if(pbRegisteredFuzzTest MATCHES "^(PBProtocolBootstrapControl|PBProtocolDescriptor|PBOuterFecWirehairV2|PBOuterFecDirectRepeat|PBModulationReferenceRaster|PBProtocolTransport|PBInterleaveReference|PBInnerFecCodeword|PBProtocolResumeState)")
            set_property(TEST "${pbRegisteredFuzzTest}" APPEND PROPERTY LABELS "parser-harness")
        endif()
    endforeach()
    unset(pbRegisteredFuzzTest)
    unset(pbRegisteredFuzzTests)
endif()
