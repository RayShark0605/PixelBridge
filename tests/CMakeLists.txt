# Unit tests (Catch2 v3 via the vcpkg manifest, see vcpkg.json).
#
# Include convention: PixelBridge-owned headers use quoted includes,
# third-party headers use angle-bracket includes. The /external:* flags in
# PbAddUnitTest exempt only third-party headers from the strict warning
# policy; owned sources keep /W4 and /WX.

find_package(Catch2 3 CONFIG REQUIRED)

function(PbAddUnitTest targetName)
    add_executable(${targetName} ${ARGN})
    target_link_libraries(${targetName}
        PRIVATE
            PB::CompilerSettings
            Catch2::Catch2WithMain)
    if(MSVC)
        target_compile_options(${targetName}
            PRIVATE
                /external:anglebrackets
                /external:W0)
    endif()
    add_test(NAME ${targetName} COMMAND ${targetName})
    # Registration for the centralized MSVC ASan runtime copy below: in a
    # fuzz-enabled tree the core libraries are /fsanitize=address
    # instrumented, so every unit-test executable that pulls them in must
    # have the ASan runtime next to it at load time. GLOBAL (not DIRECTORY)
    # so the tests/-scope copy target can collect targets registered from
    # the per-suite subdirectory scopes.
    set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS ${targetName})
endfunction()

function(PbInstrumentPresentationTest targetName)
    if(PB_BUILD_FUZZERS AND MSVC)
        target_compile_options(${targetName} PRIVATE /Zi /fsanitize=address)
        target_compile_definitions(${targetName} PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
        target_link_options(${targetName} PRIVATE /DEBUG /INCREMENTAL:NO)
    elseif(PB_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${targetName} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
        target_link_options(${targetName} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

function(PbAddExactProcessTest testName targetName expectedStdout)
    if(NOT TARGET "${targetName}")
        message(FATAL_ERROR
            "Cannot register ${testName}; target ${targetName} does not exist")
    endif()

    add_test(NAME ${testName}
        COMMAND "${CMAKE_COMMAND}"
            "-DPB_TEST_EXECUTABLE=$<TARGET_FILE:${targetName}>"
            "-DPB_EXPECTED_STDOUT=${expectedStdout}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/AssertExactProcess.cmake")
endfunction()

add_subdirectory(PBCore)
add_subdirectory(PBProtocol)
add_subdirectory(PBCompression)
add_subdirectory(PBInnerFec)
add_subdirectory(PBModulation)
add_subdirectory(PBOuterFec)
add_subdirectory(PBReceiver)
add_subdirectory(PBPresentTiming)
if(TARGET PBRenderD3D)
    add_subdirectory(PBRenderD3D)
endif()
if(TARGET PBScreenRegion)
    add_subdirectory(PBScreenRegion)
endif()
if(PB_BUILD_SCREEN_REGION_GATE)
    add_subdirectory(ScreenRegionGate)
endif()
if(PB_BUILD_PRESENTATION_GATE)
    add_subdirectory(PresentationGate)
endif()
add_subdirectory(PBInterleave)
add_subdirectory(PBGoldenVector)
add_subdirectory(golden)
if(PB_BUILD_PHASE0_GATE)
    add_subdirectory(Phase0Gate)
endif()
if(TARGET PBProtocolDumpCore AND TARGET PBFrameInspectorCore)
    add_subdirectory(tools)
endif()
add_subdirectory(CMake)

# Centralized MSVC ASan runtime copy for the unit-test executables. In the
# fuzz-enabled tree, fuzz/CMakeLists.txt compiles the core libraries
# (PBProtocol et al.) with /Zi /fsanitize=address; a full build then relinks
# every test executable against those instrumented objects, and each one
# imports the MSVC ASan runtime at load time (a missing DLL surfaces as a
# 0xC0000135 startup failure in ctest). Copy the runtime next to each
# registered test executable exactly when that instrumentation is active
# (mirrors the POST_BUILD copies in fuzz/CMakeLists.txt). fuzz/ is
# processed before tests/ (top-level order), so the option query below sees
# the instrumentation state.
if(MSVC AND TARGET PBProtocol)
    get_target_property(pbProtocolCompileOptions PBProtocol COMPILE_OPTIONS)
    if(pbProtocolCompileOptions MATCHES "/fsanitize=address")
        get_filename_component(pbMsvcCompilerDirectory
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY)
        set(pbMsvcAsanRuntime
            "${pbMsvcCompilerDirectory}/clang_rt.asan_dynamic-x86_64.dll")
        if(NOT EXISTS "${pbMsvcAsanRuntime}")
            message(FATAL_ERROR
                "MSVC AddressSanitizer runtime was not found: "
                "${pbMsvcAsanRuntime}")
        endif()
        get_property(pbUnitTestTargets GLOBAL PROPERTY PB_UNIT_TEST_TARGETS)
        # PBProtocolNoAllocationDigestProbe is created with a plain
        # add_executable and also links the instrumented PBProtocol.
        list(APPEND pbUnitTestTargets PBProtocolNoAllocationDigestProbe)
        # The golden harness and tool executables are plain executables
        # that pull the instrumented protocol stack into their link; the
        # if(TARGET) guard below skips any that are not built in this
        # configuration (tools are optional).
        list(APPEND pbUnitTestTargets
            PBGoldenVectorCheck
            PBProtocolDump
            PBFrameInspector
            PBVectorGen)
        # add_custom_command(TARGET ...) must live in the directory that
        # created the target, so a single all-of-ALL copy target created in
        # this directory places the runtime next to every test executable
        # instead of one POST_BUILD copy per suite directory.
        set(pbAsanCopyCommands)
        foreach(pbAsanTestTarget IN LISTS pbUnitTestTargets)
            if(TARGET ${pbAsanTestTarget})
                # create_directory first: this ALL target may run before the
                # test executable's output directory exists, and a copy into
                # a missing path would create a stray file named after the
                # configuration instead of a directory (MSB3191 for the
                # real target).
                list(APPEND pbAsanCopyCommands
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${pbAsanTestTarget}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${pbMsvcAsanRuntime}"
                        "$<TARGET_FILE_DIR:${pbAsanTestTarget}>")
            endif()
        endforeach()
        # The app executables link the same instrumented protocol libraries
        # (PBProtocol, PBReceiver), so they import the same ASan runtime and
        # need it next to their own executable as well.
        foreach(pbAsanAppTarget IN ITEMS PixelBridgeEncoder PixelBridgeDecoder)
            if(TARGET ${pbAsanAppTarget})
                list(APPEND pbAsanCopyCommands
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${pbAsanAppTarget}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${pbMsvcAsanRuntime}"
                        "$<TARGET_FILE_DIR:${pbAsanAppTarget}>")
            endif()
        endforeach()
        unset(pbAsanAppTarget)
        add_custom_target(PBTestAsanRuntimeCopy
            ALL
            ${pbAsanCopyCommands}
            VERBATIM)
        unset(pbAsanCopyCommands)
        unset(pbAsanTestTarget)
        unset(pbUnitTestTargets)
        unset(pbMsvcAsanRuntime)
        unset(pbMsvcCompilerDirectory)
    endif()
    unset(pbProtocolCompileOptions)
endif()

if(TARGET PixelBridgeEncoder)
    PbAddExactProcessTest(
        PixelBridgeEncoderSmoke
        PixelBridgeEncoder
        "PixelBridgeEncoder ${PROJECT_VERSION} (protocol 1.0)")
    if(WIN32)
        set(pbEncoderInvalidArguments
            "--unknown"
            "--frames|1"
            "--data-window|--frames"
            "--data-window|--frames|0"
            "--data-window|--frames|-1"
            "--data-window|--frames|1000001"
            "--data-window|--frames|18446744073709551616"
            "--data-window|--frames|1x"
            "--data-window|--data-window"
            "--data-window|--frames|1|--frames|2"
            "--data-window|--telemetry")
        set(pbEncoderArgumentIndex 0)
        foreach(pbEncoderArguments IN LISTS pbEncoderInvalidArguments)
            add_test(NAME "PixelBridgeEncoderRejectsArguments.${pbEncoderArgumentIndex}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DPB_TEST_EXECUTABLE=$<TARGET_FILE:PixelBridgeEncoder>"
                    "-DPB_EXPECTED_EXIT=2"
                    "-DPB_TEST_ARGS_ENCODED=${pbEncoderArguments}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/AssertProcessExit.cmake")
            math(EXPR pbEncoderArgumentIndex "${pbEncoderArgumentIndex} + 1")
        endforeach()
        unset(pbEncoderInvalidArguments)
        unset(pbEncoderArgumentIndex)
        unset(pbEncoderArguments)
    endif()
endif()

if(TARGET PixelBridgeDecoder)
    PbAddExactProcessTest(
        PixelBridgeDecoderSmoke
        PixelBridgeDecoder
        "PixelBridgeDecoder ${PROJECT_VERSION} (protocol 1.0)")
    if(WIN32)
        set(pbDecoderInvalidArguments "--unknown" "--select-region|--select-region" "--select-region|extra" "--help|--select-region" "--select-region|--help")
        set(pbDecoderArgumentIndex 0)
        foreach(pbDecoderArguments IN LISTS pbDecoderInvalidArguments)
            add_test(NAME "PixelBridgeDecoderRejectsArguments.${pbDecoderArgumentIndex}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DPB_TEST_EXECUTABLE=$<TARGET_FILE:PixelBridgeDecoder>"
                    "-DPB_EXPECTED_EXIT=2"
                    "-DPB_TEST_ARGS_ENCODED=${pbDecoderArguments}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/AssertProcessExit.cmake")
            math(EXPR pbDecoderArgumentIndex "${pbDecoderArgumentIndex} + 1")
        endforeach()
        add_test(NAME PixelBridgeDecoderHelp COMMAND PixelBridgeDecoder --help)
        unset(pbDecoderInvalidArguments)
        unset(pbDecoderArgumentIndex)
        unset(pbDecoderArguments)
    endif()
endif()
