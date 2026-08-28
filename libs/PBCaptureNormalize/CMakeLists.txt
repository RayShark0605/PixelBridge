add_library(PBCaptureNormalize STATIC)
add_library(PB::PBCaptureNormalize ALIAS PBCaptureNormalize)
target_sources(PBCaptureNormalize PRIVATE src/capture_runtime.cpp src/capture_timestamp.cpp src/frame_inbox.cpp src/capture_layout.cpp
    src/d3d_roi_ring.cpp src/native_support.cpp src/normalize_consumer.cpp src/diagnostic_readback.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbcapturenormalize/capture_types.h
    include/pbcapturenormalize/screen_capture_frame.h include/pbcapturenormalize/diagnostic_readback.h)
target_include_directories(PBCaptureNormalize PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(PBCaptureNormalize PUBLIC PB::PBScreenRegion
    PRIVATE PB::CompilerSettings PB::PBProtocol d3d11 dxgi d3dcompiler runtimeobject user32)
target_compile_features(PBCaptureNormalize PUBLIC cxx_std_20)
target_compile_definitions(PBCaptureNormalize PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(PBCaptureNormalize PRIVATE /external:anglebrackets /external:W0)
endif()
