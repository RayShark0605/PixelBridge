add_library(PBStorage STATIC)
add_library(PB::PBStorage ALIAS PBStorage)

target_sources(PBStorage
    PRIVATE
        src/output_file.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/pbstorage/output_file.h)

target_include_directories(PBStorage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(PBStorage
    PRIVATE
        PB::CompilerSettings
    PUBLIC
        PB::PBProtocol)

target_compile_features(PBStorage PUBLIC cxx_std_20)
target_compile_definitions(PBStorage PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBStorage PRIVATE /external:anglebrackets /external:W0)
endif()
