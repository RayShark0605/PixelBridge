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
add_subdirectory(PBCore)
add_subdirectory(PBProtocol)
