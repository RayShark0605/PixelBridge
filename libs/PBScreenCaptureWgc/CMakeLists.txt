add_library(PBScreenCaptureWgc STATIC)
add_library(PB::PBScreenCaptureWgc ALIAS PBScreenCaptureWgc)
target_sources(PBScreenCaptureWgc
    PRIVATE src/wgc_capture.cpp src/native_capture.cpp
        src/capture_internal.h src/d3d_roi_ring.h src/session_capabilities.cpp src/session_capabilities.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbscreencapturewgc/wgc_capture.h)
target_include_directories(PBScreenCaptureWgc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(PBScreenCaptureWgc
    PUBLIC PB::PBCaptureNormalize
    PRIVATE PB::CompilerSettings PB::PBProtocol d3d11 dxgi windowsapp runtimeobject user32)
target_compile_features(PBScreenCaptureWgc PUBLIC cxx_std_20)
target_compile_definitions(PBScreenCaptureWgc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(PBScreenCaptureWgc PRIVATE /external:anglebrackets /external:W0)
endif()
