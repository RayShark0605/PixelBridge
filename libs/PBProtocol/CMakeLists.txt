find_package(blake3 1.8.5 CONFIG REQUIRED)

add_library(PBProtocol STATIC)
add_library(PB::PBProtocol ALIAS PBProtocol)

target_sources(PBProtocol
    PRIVATE
        src/byte_io.cpp
        src/descriptor_binding.cpp
        src/descriptor_codec.cpp
        src/protocol_types.cpp
        src/protocol_version.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbprotocol/byte_io.h
            include/pbprotocol/checked_integer.h
            include/pbprotocol/descriptor_binding.h
            include/pbprotocol/descriptor_codec.h
            include/pbprotocol/feature_flags.h
            include/pbprotocol/protocol_result.h
            include/pbprotocol/protocol_types.h
            include/pbprotocol/protocol_version.h)

target_include_directories(PBProtocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBProtocol
    PUBLIC
        cxx_std_20)

target_link_libraries(PBProtocol
    PRIVATE
        PB::CompilerSettings
        BLAKE3::blake3)
