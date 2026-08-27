add_library(PBScreenRegion STATIC)
add_library(PB::PBScreenRegion ALIAS PBScreenRegion)
target_sources(PBScreenRegion
    PRIVATE src/screen_region.cpp src/native_backend.cpp src/screen_region_json.cpp src/screen_region_internal.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbscreenregion/screen_region.h)
target_include_directories(PBScreenRegion PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(PBScreenRegion PRIVATE PB::CompilerSettings user32 gdi32 dxgi imm32)
target_compile_features(PBScreenRegion PUBLIC cxx_std_20)
target_compile_definitions(PBScreenRegion PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(PBScreenRegion PRIVATE /external:anglebrackets /external:W0)
endif()
