add_library(PBInnerFec STATIC)
add_library(PB::PBInnerFec ALIAS PBInnerFec)

target_sources(PBInnerFec
    PRIVATE
        src/dvbs2_short_matrix.h
        src/inner_fec_profile.cpp
        src/qc_ldpc_codec.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbinnerfec/inner_fec_profile.h
            include/pbinnerfec/inner_fec_result.h
            include/pbinnerfec/qc_ldpc_codec.h)

target_include_directories(PBInnerFec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBInnerFec
    PUBLIC
        cxx_std_20)

target_link_libraries(PBInnerFec
    PRIVATE
        PB::PBProtocol
        PB::CompilerSettings)
