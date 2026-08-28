add_library(PBScreenCaptureDxgi STATIC)
add_library(PB::PBScreenCaptureDxgi ALIAS PBScreenCaptureDxgi)

target_sources(PBScreenCaptureDxgi
    PRIVATE src/dxgi_capture.cpp src/duplication_source.cpp src/native_capture.cpp
        src/capture_internal.h src/duplication_source.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbscreencapturedxgi/dxgi_capture.h)
target_include_directories(PBScreenCaptureDxgi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_include_directories(PBScreenCaptureDxgi PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBCaptureNormalize/src")
target_link_libraries(PBScreenCaptureDxgi PUBLIC PB::PBCaptureNormalize
    PRIVATE PB::CompilerSettings PB::PBProtocol d3d11 dxgi user32 wtsapi32)
target_compile_features(PBScreenCaptureDxgi PUBLIC cxx_std_20)
target_compile_definitions(PBScreenCaptureDxgi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(PBScreenCaptureDxgi PRIVATE /external:anglebrackets /external:W0)
endif()
