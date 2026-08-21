add_library(PBCore
    src/build_info.cpp)
add_library(PB::PBCore ALIAS PBCore)

target_include_directories(PBCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Version is injected from the top-level project() call so that CMake stays
# the single source of truth.
target_compile_definitions(PBCore
    PRIVATE
        PB_CORE_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(PBCore
    PUBLIC
        PB::CompilerSettings)