cmake_minimum_required(VERSION 3.24)

# vcpkg resolves manifest features while project() loads the toolchain. Define
# top-level test switches first so a production configuration does not install
# Catch2. As a subproject, the parent owns BUILD_TESTING and PixelBridge tests
# remain opt-in.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(pbTopLevelBeforeProject ON)
    set(pbBuildTestsDefault ON)
    option(BUILD_TESTING "Build the testing tree" ON)
else()
    set(pbTopLevelBeforeProject OFF)
    set(pbBuildTestsDefault OFF)
endif()

option(PB_BUILD_TESTS "Build the PixelBridge tests" ${pbBuildTestsDefault})

if(pbTopLevelBeforeProject AND BUILD_TESTING AND PB_BUILD_TESTS)
    list(FIND VCPKG_MANIFEST_FEATURES tests pbTestsFeatureIndex)
    if(pbTestsFeatureIndex EQUAL -1)
        list(APPEND VCPKG_MANIFEST_FEATURES tests)
    endif()
endif()

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

if(PROJECT_IS_TOP_LEVEL)
    set(pbBuildAppsDefault ON)
else()
    set(pbBuildAppsDefault OFF)
endif()

option(PB_BUILD_APPS "Build the PixelBridge applications" ${pbBuildAppsDefault})
option(PB_BUILD_TOOLS "Build PixelBridge command-line tools" OFF)
option(PB_BUILD_FUZZERS "Build PixelBridge fuzz targets" OFF)
option(PB_BUILD_BENCHMARKS "Build PixelBridge benchmark targets" OFF)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PBCompilerSettings)
include(PBTargetPolicy)

add_subdirectory(libs)
PbValidateCoreTargetBoundaries("${CMAKE_CURRENT_SOURCE_DIR}/libs")

if(PB_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(PB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(PB_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

if(PB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTING AND PB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

unset(pbBuildAppsDefault)
unset(pbBuildTestsDefault)
unset(pbTopLevelBeforeProject)
unset(pbTestsFeatureIndex)
