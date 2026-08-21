find_package(blake3 1.8.5 CONFIG REQUIRED)

add_library(PBProtocol
    src/byte_io.cpp
    src/descriptor_binding.cpp
    src/descriptor_codec.cpp
    src/protocol_types.cpp
    src/protocol_version.cpp)
add_library(PB::PBProtocol ALIAS PBProtocol)

target_include_directories(PBProtocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(PBProtocol
    PRIVATE
        BLAKE3::blake3
    PUBLIC
        PB::CompilerSettings
        PB::PBCore)
