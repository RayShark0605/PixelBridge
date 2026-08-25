find_package(blake3 1.8.5 CONFIG REQUIRED)

add_library(PBProtocol STATIC)
add_library(PB::PBProtocol ALIAS PBProtocol)

target_sources(PBProtocol
    PRIVATE
        src/blake3_digest.cpp
        src/bootstrap_control_codec.cpp
        src/byte_io.cpp
        src/control_fragment_codec.cpp
        src/control_plane_receiver.cpp
        src/crc32c.cpp
        src/descriptor_binding.cpp
        src/descriptor_codec.cpp
        src/orphan_transport_block_cache.cpp
        src/output_reservation.cpp
        src/pmr_byte_buffer.h
        src/protocol_types.cpp
        src/protocol_version.cpp
        src/resume_state.cpp
        src/session_random.cpp
        src/session_random_internal.h
        src/session_registry.cpp
        src/transport_block_codec.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbprotocol/blake3_digest.h
            include/pbprotocol/bootstrap_control_codec.h
            include/pbprotocol/byte_io.h
            include/pbprotocol/checked_integer.h
            include/pbprotocol/control_fragment_codec.h
            include/pbprotocol/control_plane_receiver.h
            include/pbprotocol/crc32c.h
            include/pbprotocol/descriptor_binding.h
            include/pbprotocol/descriptor_codec.h
            include/pbprotocol/feature_flags.h
            include/pbprotocol/orphan_transport_block_cache.h
            include/pbprotocol/output_reservation.h
            include/pbprotocol/protocol_result.h
            include/pbprotocol/protocol_types.h
            include/pbprotocol/protocol_version.h
            include/pbprotocol/resume_state.h
            include/pbprotocol/session_random.h
            include/pbprotocol/session_registry.h
            include/pbprotocol/transport_block_codec.h)

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

if(WIN32)
    target_link_libraries(PBProtocol
        PRIVATE
            bcrypt)
endif()
