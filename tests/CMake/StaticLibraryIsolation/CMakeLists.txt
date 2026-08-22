cmake_minimum_required(VERSION 3.24)

project(PBStaticLibraryIsolation LANGUAGES CXX)

enable_testing()

if(NOT DEFINED PB_SOURCE_DIR)
    message(FATAL_ERROR "PB_SOURCE_DIR is required")
endif()

set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(PB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(PB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory("${PB_SOURCE_DIR}" PixelBridge)

foreach(staticTarget IN ITEMS PBCore PBProtocol)
    get_target_property(targetType "${staticTarget}" TYPE)
    if(NOT targetType STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "${staticTarget} changed type under BUILD_SHARED_LIBS=ON: ${targetType}")
    endif()

    get_target_property(interfaceCompileOptions
        "${staticTarget}"
        INTERFACE_COMPILE_OPTIONS)
    if(interfaceCompileOptions)
        string(JOIN ";" joinedInterfaceCompileOptions ${interfaceCompileOptions})
        if(joinedInterfaceCompileOptions MATCHES "/WX|/W[0-4]|/permissive-")
            message(FATAL_ERROR
                "${staticTarget} leaks internal compiler options: "
                "${joinedInterfaceCompileOptions}")
        endif()
    endif()
endforeach()

get_target_property(protocolLinkLibraries PBProtocol LINK_LIBRARIES)
get_target_property(protocolInterfaceLinkLibraries
    PBProtocol
    INTERFACE_LINK_LIBRARIES)
string(JOIN ";" protocolDependencyDeclarations
    ${protocolLinkLibraries}
    ${protocolInterfaceLinkLibraries})
if(protocolDependencyDeclarations MATCHES "PB::PBCore|(^|;)PBCore($|;)")
    message(FATAL_ERROR
        "PBProtocol unexpectedly depends on PBCore: "
        "${protocolDependencyDeclarations}")
endif()

get_target_property(protocolInterfaceIncludes
    PBProtocol
    INTERFACE_INCLUDE_DIRECTORIES)
string(JOIN ";" protocolIncludeDeclarations ${protocolInterfaceIncludes})
if(protocolIncludeDeclarations MATCHES "PBCore")
    message(FATAL_ERROR
        "PBProtocol exposes PBCore include paths: ${protocolIncludeDeclarations}")
endif()

add_executable(PBCoreOnlyConsumer core_consumer.cpp)
target_link_libraries(PBCoreOnlyConsumer PRIVATE PB::PBCore)

add_executable(PBProtocolOnlyConsumer protocol_consumer.cpp)
target_link_libraries(PBProtocolOnlyConsumer PRIVATE PB::PBProtocol)

add_test(NAME PBCoreOnlyConsumerRun COMMAND PBCoreOnlyConsumer)
add_test(NAME PBProtocolOnlyConsumerRun COMMAND PBProtocolOnlyConsumer)
