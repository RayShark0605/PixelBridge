add_library(PBPresentTiming STATIC)
add_library(PB::PBPresentTiming ALIAS PBPresentTiming)
target_sources(PBPresentTiming
    PRIVATE src/present_timing.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/pbpresenttiming/present_timing.h)
target_include_directories(PBPresentTiming PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(PBPresentTiming PRIVATE PB::CompilerSettings PB::PBProtocol)
target_compile_features(PBPresentTiming PUBLIC cxx_std_20)
