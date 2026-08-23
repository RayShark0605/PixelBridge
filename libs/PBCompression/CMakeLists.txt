find_package(zstd 1.5.7 CONFIG REQUIRED)

add_library(PBCompression STATIC)
add_library(PB::PBCompression ALIAS PBCompression)

target_sources(PBCompression
    PRIVATE
        src/compression_internal.h
        src/segment_compression.cpp
        src/segment_decompression.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbcompression/compression_error.h
            include/pbcompression/segment_compression.h
            include/pbcompression/segment_decompression.h)

target_include_directories(PBCompression
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBCompression
    PUBLIC
        cxx_std_20)

target_link_libraries(PBCompression
    PUBLIC
        PB::PBProtocol
    PRIVATE
        PB::CompilerSettings
        zstd::libzstd)