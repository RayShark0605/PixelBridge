add_library(PBRemoteVisualSimulator STATIC)
add_library(PB::PBRemoteVisualSimulator ALIAS PBRemoteVisualSimulator)

target_sources(PBRemoteVisualSimulator
    PRIVATE
        src/channel_transform.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbremotevisualsimulator/channel_transform.h)

target_include_directories(PBRemoteVisualSimulator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBRemoteVisualSimulator
    PUBLIC
        cxx_std_20)

target_link_libraries(PBRemoteVisualSimulator
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)
