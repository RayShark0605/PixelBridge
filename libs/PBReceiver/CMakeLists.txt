add_library(PBReceiver STATIC)
add_library(PB::PBReceiver ALIAS PBReceiver)

target_sources(PBReceiver
    PRIVATE
        src/receiver_ingress.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbreceiver/receiver_ingress.h
            include/pbreceiver/receiver_result.h)

target_include_directories(PBReceiver
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBReceiver
    PUBLIC
        cxx_std_20)

target_link_libraries(PBReceiver
    PUBLIC
        PB::PBCompression
        PB::PBOuterFec
        PB::PBProtocol
    PRIVATE
        PB::CompilerSettings)
