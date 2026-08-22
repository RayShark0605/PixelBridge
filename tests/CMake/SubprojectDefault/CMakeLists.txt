cmake_minimum_required(VERSION 3.24)

project(PBSubprojectDefault LANGUAGES CXX)

if(NOT DEFINED PB_SOURCE_DIR)
    message(FATAL_ERROR "PB_SOURCE_DIR is required")
endif()

include(CTest)
if(NOT BUILD_TESTING)
    message(FATAL_ERROR "The parent test tree must remain enabled")
endif()

set(CMAKE_DISABLE_FIND_PACKAGE_Catch2 ON CACHE BOOL "" FORCE)
set(PB_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_test(NAME PBParentSmoke COMMAND "${CMAKE_COMMAND}" -E true)

add_subdirectory("${PB_SOURCE_DIR}" PixelBridge)

if(PB_BUILD_TESTS)
    message(FATAL_ERROR "PixelBridge tests must default to OFF as a subproject")
endif()

if(TARGET PBCoreTests OR TARGET PBProtocolTests)
    message(FATAL_ERROR "PixelBridge unexpectedly created test targets")
endif()

add_executable(PBSubprojectConsumer consumer.cpp)
target_link_libraries(PBSubprojectConsumer PRIVATE PB::PBProtocol)
add_test(NAME PBSubprojectConsumerRun COMMAND PBSubprojectConsumer)
