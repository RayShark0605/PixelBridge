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
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbmodulation/frame_io.h
            include/pbmodulation/modulation_result.h
            include/pbmodulation/reference_raster.h
            include/pbmodulation/reference_visual_profile.h
            include/pbmodulation/local_desktop_bootstrap.h
            include/pbmodulation/local_desktop_decode.h)

target_include_directories(PBModulation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBModulation
    PUBLIC
        cxx_std_20)

target_link_libraries(PBModulation
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol
        PNG::PNG)

# libpng headers are third-party: keep them out of the strict warning policy
# (angle-bracket include + /external exemption), mirroring the test targets.
if(MSVC)
    target_compile_options(PBModulation
        PRIVATE
            /external:anglebrackets
            /external:W0)
endif()
