find_package(wirehair 2.0 CONFIG REQUIRED)

add_library(PBOuterFec STATIC)
add_library(PB::PBOuterFec ALIAS PBOuterFec)

target_sources(PBOuterFec
    PRIVATE
        src/wirehair_v2.cpp
        src/wirehair_v2_backend.cpp
        src/wirehair_v2_backend.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbouterfec/outer_fec_result.h
            include/pbouterfec/wirehair_v2.h)

target_include_directories(PBOuterFec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBOuterFec
    PUBLIC
        cxx_std_20)

target_link_libraries(PBOuterFec
    PUBLIC
        PB::PBProtocol
    PRIVATE
        PB::CompilerSettings
        wirehair::wirehair)

if(MSVC)
    target_compile_options(PBOuterFec
        PRIVATE
            /external:anglebrackets
            /external:W0)
endif()
