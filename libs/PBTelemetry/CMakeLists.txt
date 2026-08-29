add_library(PBTelemetry STATIC)
add_library(PB::PBTelemetry ALIAS PBTelemetry)
target_sources(PBTelemetry PRIVATE src/telemetry.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbtelemetry/telemetry.h)
target_include_directories(PBTelemetry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(PBTelemetry PUBLIC PB::PBCaptureNormalize PB::PBPresentTiming PB::PBDesktopLevelsReference
    PRIVATE PB::CompilerSettings PB::PBProtocol)
target_compile_features(PBTelemetry PUBLIC cxx_std_20)
target_compile_definitions(PBTelemetry PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBTelemetry PRIVATE /external:anglebrackets /external:W0)
endif()
