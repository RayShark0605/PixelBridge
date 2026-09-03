find_package(PNG 1.6.58 CONFIG REQUIRED)

add_library(PBModulation STATIC)
add_library(PB::PBModulation ALIAS PBModulation)

target_sources(PBModulation
    PRIVATE
        src/raster_internal.h
        src/frame_io.cpp
        src/reference_raster.cpp
        src/reference_visual_profile.cpp
        src/bootstrap_rs.cpp
        src/local_desktop_bootstrap.cpp
        src/local_desktop_decode.cpp
        src/local_desktop_internal.h
        src/luma_reader.h
        src/desktop_levels.cpp
        src/desktop_levels_internal.h
        src/shape_chroma.cpp
        src/shape_chroma_internal.h
        src/remote_visual.cpp
        src/remote_visual_low_fps.cpp
        src/unified_visual_profile.cpp
        src/unified_visual.cpp
        src/visual_temporal.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbmodulation/frame_io.h
            include/pbmodulation/modulation_result.h
            include/pbmodulation/reference_raster.h
            include/pbmodulation/reference_visual_profile.h
            include/pbmodulation/local_desktop_bootstrap.h
            include/pbmodulation/local_desktop_decode.h
            include/pbmodulation/desktop_levels.h
            include/pbmodulation/shape_chroma.h
            include/pbmodulation/remote_visual.h
            include/pbmodulation/remote_visual_low_fps.h
            include/pbmodulation/unified_visual_profile.h
            include/pbmodulation/unified_visual_mapping.h
            include/pbmodulation/unified_visual.h
            include/pbmodulation/visual_temporal.h)

target_include_directories(PBModulation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBModulation
    PUBLIC
        cxx_std_20)

target_link_libraries(PBModulation
    PUBLIC
        PB::PBProtocol
    PRIVATE
        PB::CompilerSettings
        PB::PBInnerFec
        PB::PBInterleave
        PNG::PNG)

# libpng headers are third-party: keep them out of the strict warning policy
# (angle-bracket include + /external exemption), mirroring the test targets.
if(MSVC)
    target_compile_options(PBModulation
        PRIVATE
            /external:anglebrackets
            /external:W0)
endif()
