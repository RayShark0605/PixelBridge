add_library(PBRenderD3D STATIC)
add_library(PB::PBRenderD3D ALIAS PBRenderD3D)
target_sources(PBRenderD3D
    PRIVATE src/data_window.cpp src/native_backend.cpp src/presentation_backend.h src/presentation_json.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/pbrenderd3d/data_window.h)
target_include_directories(PBRenderD3D PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(PBRenderD3D
    PUBLIC PB::PBPresentTiming
    PRIVATE PB::CompilerSettings PB::PBProtocol d3d11 dxgi d3dcompiler user32)
target_compile_features(PBRenderD3D PUBLIC cxx_std_20)
target_compile_definitions(PBRenderD3D PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(PBRenderD3D PRIVATE /external:anglebrackets /external:W0)
endif()
