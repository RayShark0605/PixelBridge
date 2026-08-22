add_library(PBCore STATIC)
add_library(PB::PBCore ALIAS PBCore)

target_sources(PBCore
    PRIVATE
        src/build_info.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/pbcore/build_info.h)

target_include_directories(PBCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBCore
    PUBLIC
        cxx_std_20)

# Version is injected from the top-level project() call so that CMake stays
# the single source of truth.
target_compile_definitions(PBCore
    PRIVATE
        PB_CORE_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(PBCore
    PRIVATE
        PB::CompilerSettings)
