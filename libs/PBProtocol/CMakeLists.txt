add_library(PBProtocol
    src/protocol_version.cpp)
add_library(PB::PBProtocol ALIAS PBProtocol)

target_include_directories(PBProtocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(PBProtocol
    PUBLIC
        PB::CompilerSettings
        PB::PBCore)