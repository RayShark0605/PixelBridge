set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/demod_compute.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/remote_visual_low_fps_compute.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unified_visual_compute.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/demod_compute.hlsl" PB_DEMOD_D3D11_HLSL)
configure_file(src/demod_shader_source.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/demod_shader_source.h" @ONLY)
unset(PB_DEMOD_D3D11_HLSL)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/remote_visual_low_fps_compute.hlsl" PB_REMOTE_VISUAL_LOW_FPS_D3D11_HLSL)
configure_file(src/remote_visual_low_fps_shader_source.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/remote_visual_low_fps_shader_source.h" @ONLY)
unset(PB_REMOTE_VISUAL_LOW_FPS_D3D11_HLSL)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/unified_visual_compute.hlsl" PB_UNIFIED_VISUAL_D3D11_HLSL)
configure_file(src/unified_visual_shader_source.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/unified_visual_shader_source.h" @ONLY)
unset(PB_UNIFIED_VISUAL_D3D11_HLSL)

add_library(PBDemodD3D11 STATIC)
add_library(PB::PBDemodD3D11 ALIAS PBDemodD3D11)
target_sources(PBDemodD3D11 PRIVATE src/demodulator.cpp src/capture_demodulator.cpp src/demod_compute.hlsl
    src/remote_visual_low_fps_compute.hlsl
    src/unified_visual_compute.hlsl
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbdemodd3d11/demodulator.h include/pbdemodd3d11/capture_demodulator.h)
set_source_files_properties(src/demod_compute.hlsl src/remote_visual_low_fps_compute.hlsl
    src/unified_visual_compute.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(PBDemodD3D11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(PBDemodD3D11 PUBLIC PB::PBCaptureNormalize PB::PBDesktopLevelsReference
    PRIVATE PB::CompilerSettings PB::PBProtocol PB::PBInterleave d3d11 dxgi d3dcompiler)
target_compile_features(PBDemodD3D11 PUBLIC cxx_std_20)
target_compile_definitions(PBDemodD3D11 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBDemodD3D11 PRIVATE /external:anglebrackets /external:W0)
endif()
