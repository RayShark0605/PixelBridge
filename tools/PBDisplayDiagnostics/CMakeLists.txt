cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PixelBridgeDisplayDiagnostics LANGUAGES CXX)
    set(PB_TREAT_WARNINGS_AS_ERRORS ON)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PBCompilerSettings.cmake")
endif()

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "PBDisplayDiagnostics requires Windows/MSVC")
endif()

get_filename_component(pbDiagnosticRoot "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${pbDiagnosticRoot}"
    OUTPUT_VARIABLE pbDiagnosticBaseCommit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/main.cpp" pbDiagnosticSourceHash)
file(SHA256 "${pbDiagnosticRoot}/apps/common/monitor_catalog.cpp" pbDiagnosticCatalogHash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp" "${pbDiagnosticRoot}/apps/common/monitor_catalog.cpp")

# Compile the actual catalog, not a relaxed copy. The independent metadata
# walk below it continues past missing DXGI/DPI/mode data to expose the cause.
add_executable(PBDisplayDiagnostics WIN32 main.cpp
    "${pbDiagnosticRoot}/apps/common/monitor_catalog.cpp"
    "${pbDiagnosticRoot}/cmake/PixelBridge.PerMonitorV2.manifest")
set_target_properties(PBDisplayDiagnostics PROPERTIES AUTOMOC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(PBDisplayDiagnostics PRIVATE PB::CompilerSettings user32 shcore dxgi)
target_include_directories(PBDisplayDiagnostics PRIVATE
    "${pbDiagnosticRoot}/apps/common"
    "${pbDiagnosticRoot}/libs/PBProtocol/include"
    "${pbDiagnosticRoot}/libs/PBModulation/include"
    "${pbDiagnosticRoot}/libs/PBCaptureNormalize/include"
    "${pbDiagnosticRoot}/libs/PBDesktopLevelsReference/include"
    "${pbDiagnosticRoot}/libs/PBPresentTiming/include"
    "${pbDiagnosticRoot}/libs/PBScreenRegion/include"
    "${pbDiagnosticRoot}/libs/PBTelemetry/include")
target_compile_definitions(PBDisplayDiagnostics PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_DIAGNOSTIC_BASE_COMMIT=\"${pbDiagnosticBaseCommit}\""
    "PB_DIAGNOSTIC_SOURCE_HASH=\"${pbDiagnosticSourceHash}\""
    "PB_DIAGNOSTIC_CATALOG_HASH=\"${pbDiagnosticCatalogHash}\"")
target_link_options(PBDisplayDiagnostics PRIVATE /ENTRY:wmainCRTStartup)
