# Test-only operator entry. No CTest registration: a live run requires the
# user's remote sender and a fresh protected-monitor preflight.
add_executable(PBUnifiedRemoteGate remote_decoder_gate.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBUnifiedRemoteGate PRIVATE PB::CompilerSettings PB::PBApplication user32)
execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE pbUnifiedRemoteCommit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(PBUnifiedRemoteGate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_GIT_COMMIT=\"${pbUnifiedRemoteCommit}\"")
if(MSVC)
    target_compile_options(PBUnifiedRemoteGate PRIVATE /external:anglebrackets /external:W0)
endif()
unset(pbUnifiedRemoteCommit)
