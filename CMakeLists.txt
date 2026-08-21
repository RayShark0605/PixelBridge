cmake_minimum_required(VERSION 3.24)

project(PixelBridge
    VERSION 0.1.0
    DESCRIPTION "High-performance one-way file transfer through visible desktop and video pixels"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning policy for PixelBridge-owned sources (see cmake/PBCompilerSettings.cmake).
option(PB_TREAT_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors for PixelBridge-owned sources"
    ON)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PBCompilerSettings)

add_subdirectory(libs)
add_subdirectory(apps)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()