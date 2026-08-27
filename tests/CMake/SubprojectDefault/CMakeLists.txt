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

# The scratch consumer builds run without the vcpkg manifest toolchain, so
# they must inherit the per-config imported-library mapping that vcpkg.cmake
# applies for the main tree; otherwise CMake's default mapping links the
# debug-CRT vcpkg libraries (wirehair, blake3) into RelWithDebInfo and
# MinSizeRel consumers and the link fails with LNK2038.
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "MinSizeRel;Release;None;")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release;None;")

add_test(NAME PBParentSmoke COMMAND "${CMAKE_COMMAND}" -E true)

add_subdirectory("${PB_SOURCE_DIR}" PixelBridge)

if(PB_BUILD_TESTS)
    message(FATAL_ERROR "PixelBridge tests must default to OFF as a subproject")
endif()

if(TARGET PBCoreTests OR TARGET PBProtocolTests OR TARGET PBOuterFecTests OR
        TARGET PBPresentTimingTests OR TARGET PBRenderD3DTests OR TARGET PBPresentationGate OR
        TARGET PBScreenRegionTests OR TARGET PBScreenRegionDpiProbe OR TARGET PBScreenRegionDecoderHostProbe OR TARGET PBScreenRegionGate)
    message(FATAL_ERROR "PixelBridge unexpectedly created test targets")
endif()

if(PB_BUILD_PRESENTATION_GATE)
    message(FATAL_ERROR "Interactive presentation gates must default to OFF as a subproject")
endif()

if(PB_BUILD_SCREEN_REGION_GATE)
    message(FATAL_ERROR "Interactive screen-region gates must default to OFF as a subproject")
endif()
if(WIN32 AND NOT TARGET PB::PBScreenRegion)
    message(FATAL_ERROR "The Windows screen-region core library must not depend on applications or tests being enabled")
endif()

add_executable(PBSubprojectConsumer consumer.cpp)
target_link_libraries(PBSubprojectConsumer PRIVATE PB::PBOuterFec)
add_test(NAME PBSubprojectConsumerRun COMMAND PBSubprojectConsumer)
