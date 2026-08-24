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
add_subdirectory(PBOuterFec)
add_subdirectory(PBReceiver)
add_subdirectory(CMake)

if(TARGET PixelBridgeEncoder)
    PbAddExactProcessTest(
        PixelBridgeEncoderSmoke
        PixelBridgeEncoder
        "PixelBridgeEncoder ${PROJECT_VERSION} (protocol 1.0)")
endif()

if(TARGET PixelBridgeDecoder)
    PbAddExactProcessTest(
        PixelBridgeDecoderSmoke
        PixelBridgeDecoder
        "PixelBridgeDecoder ${PROJECT_VERSION} (protocol 1.0)")
endif()
