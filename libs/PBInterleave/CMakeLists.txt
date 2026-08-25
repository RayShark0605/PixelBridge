add_library(PBInterleave STATIC)
add_library(PB::PBInterleave ALIAS PBInterleave)

target_sources(PBInterleave
    PRIVATE
        src/interleave_reference.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbinterleave/interleave_reference.h)

target_include_directories(PBInterleave
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBInterleave
    PUBLIC
        cxx_std_20)

target_link_libraries(PBInterleave
    PRIVATE
        PB::CompilerSettings)
