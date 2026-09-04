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
# MSVC limits one string literal to 16 KiB. Adjacent bounded raw literals keep
# the compiler input byte-identical without imposing that limit on the shader.
set(PB_UNIFIED_VISUAL_D3D11_LITERALS "R\"PBHLSL(\n)PBHLSL\"\n")
string(LENGTH "${PB_UNIFIED_VISUAL_D3D11_HLSL}" pbUnifiedSourceLength)
set(pbUnifiedSourceOffset 0)
while(pbUnifiedSourceOffset LESS pbUnifiedSourceLength)
    string(SUBSTRING "${PB_UNIFIED_VISUAL_D3D11_HLSL}" ${pbUnifiedSourceOffset} 8192 pbUnifiedSourceChunk)
    string(APPEND PB_UNIFIED_VISUAL_D3D11_LITERALS "R\"PBHLSL(${pbUnifiedSourceChunk})PBHLSL\"\n")
    math(EXPR pbUnifiedSourceOffset "${pbUnifiedSourceOffset} + 8192")
endwhile()
configure_file(src/unified_visual_shader_source.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/unified_visual_shader_source.h" @ONLY)
unset(PB_UNIFIED_VISUAL_D3D11_HLSL)
unset(PB_UNIFIED_VISUAL_D3D11_LITERALS)
unset(pbUnifiedSourceLength)
unset(pbUnifiedSourceOffset)
unset(pbUnifiedSourceChunk)

# This is a private build dependency, not an installed tool or runtime fallback.
add_executable(PBDemodShaderCompiler EXCLUDE_FROM_ALL src/compile_shaders.cpp)
target_include_directories(PBDemodShaderCompiler PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(PBDemodShaderCompiler PRIVATE PB::CompilerSettings d3dcompiler)
target_compile_definitions(PBDemodShaderCompiler PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBDemodShaderCompiler PRIVATE /external:anglebrackets /external:W0)
endif()

# Separate outputs prevent Release/Debug/ASan build configurations from sharing a writer.
set(pbDemodShaderBytecode "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/demod_shader_bytecode.h")
add_custom_command(OUTPUT "${pbDemodShaderBytecode}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
    COMMAND "$<TARGET_FILE:PBDemodShaderCompiler>" "${pbDemodShaderBytecode}"
    DEPENDS PBDemodShaderCompiler
        src/demod_shader_entries.inc src/demod_compute.hlsl
        src/remote_visual_low_fps_compute.hlsl src/unified_visual_compute.hlsl
    COMMENT "Precompiling the 12 PBDemodD3D11 compute shader entry points"
    VERBATIM)
add_custom_target(PBDemodShaderBytecode DEPENDS "${pbDemodShaderBytecode}")
unset(pbDemodShaderBytecode)

add_library(PBDemodD3D11 STATIC)
add_library(PB::PBDemodD3D11 ALIAS PBDemodD3D11)
add_dependencies(PBDemodD3D11 PBDemodShaderBytecode)
target_sources(PBDemodD3D11 PRIVATE src/demodulator.cpp src/capture_demodulator.cpp src/demod_compute.hlsl
    src/remote_visual_low_fps_compute.hlsl
    src/unified_visual_compute.hlsl
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbdemodd3d11/demodulator.h include/pbdemodd3d11/capture_demodulator.h)
set_source_files_properties(src/demod_compute.hlsl src/remote_visual_low_fps_compute.hlsl
    src/unified_visual_compute.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(PBDemodD3D11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
target_link_libraries(PBDemodD3D11 PUBLIC PB::PBCaptureNormalize PB::PBDesktopLevelsReference
    PRIVATE PB::CompilerSettings PB::PBProtocol PB::PBInterleave d3d11 dxgi)
target_compile_features(PBDemodD3D11 PUBLIC cxx_std_20)
target_compile_definitions(PBDemodD3D11 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBDemodD3D11 PRIVATE /external:anglebrackets /external:W0)
endif()
