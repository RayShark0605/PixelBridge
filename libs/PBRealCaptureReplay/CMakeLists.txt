add_library(PBRealCaptureReplay STATIC)
add_library(PB::PBRealCaptureReplay ALIAS PBRealCaptureReplay)
target_sources(PBRealCaptureReplay PRIVATE src/replay_file.cpp src/replay_v2.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbrealcapturereplay/replay_file.h
        include/pbrealcapturereplay/replay_v2.h)
target_include_directories(PBRealCaptureReplay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(PBRealCaptureReplay PUBLIC PB::PBCaptureNormalize PB::PBPresentTiming
    PRIVATE PB::CompilerSettings PB::PBProtocol)
target_compile_features(PBRealCaptureReplay PUBLIC cxx_std_20)
target_compile_definitions(PBRealCaptureReplay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBRealCaptureReplay PRIVATE /external:anglebrackets /external:W0)
endif()
