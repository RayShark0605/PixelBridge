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

# The scratch consumer builds run without the vcpkg manifest toolchain, so
# they must inherit the per-config imported-library mapping that vcpkg.cmake
# applies for the main tree; otherwise CMake's default mapping links the
# debug-CRT vcpkg libraries (wirehair, blake3) into RelWithDebInfo and
# MinSizeRel consumers and the link fails with LNK2038.
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "MinSizeRel;Release;None;")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release;None;")

add_subdirectory("${PB_SOURCE_DIR}" PixelBridge)

foreach(staticTarget IN ITEMS
        PBCore PBProtocol PBCompression PBModulation PBOuterFec PBReceiver)
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

add_executable(PBCompressionOnlyConsumer compression_consumer.cpp)
target_link_libraries(PBCompressionOnlyConsumer PRIVATE PB::PBCompression)

add_executable(PBModulationOnlyConsumer modulation_consumer.cpp)
target_link_libraries(PBModulationOnlyConsumer PRIVATE PB::PBModulation)
# The scratch build runs without the vcpkg manifest toolchain, so the
# libpng shared dependency and its zlib dependency are not auto-copied by
# vcpkg's post-build step; replicate those copies for the consumer
# executable (debug builds need the debug-CRT variants of both DLLs, and
# the debug zlib is not on the default search path). The PNG::png_shared
# imported target is not visible from this directory scope (imported
# targets are local to their creating directory), so the DLL locations are
# derived from the PNG_DIR hint, which the vcpkg layout pins to
# <install root>/share/png.
get_filename_component(pbPngShareDirectory "${PNG_DIR}" DIRECTORY)
get_filename_component(pbPngInstallRoot "${pbPngShareDirectory}" DIRECTORY)
add_custom_command(TARGET PBModulationOnlyConsumer
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${pbPngInstallRoot}/debug/bin/libpng16d.dll,${pbPngInstallRoot}/bin/libpng16.dll>"
        "$<IF:$<CONFIG:Debug>,${pbPngInstallRoot}/debug/bin/zd.dll,${pbPngInstallRoot}/bin/z.dll>"
        "$<TARGET_FILE_DIR:PBModulationOnlyConsumer>")
unset(pbPngShareDirectory)
unset(pbPngInstallRoot)

add_executable(PBOuterFecOnlyConsumer outer_fec_consumer.cpp)
target_link_libraries(PBOuterFecOnlyConsumer PRIVATE PB::PBOuterFec)

add_executable(PBReceiverOnlyConsumer receiver_consumer.cpp)
target_link_libraries(PBReceiverOnlyConsumer PRIVATE PB::PBReceiver)

add_test(NAME PBCoreOnlyConsumerRun COMMAND PBCoreOnlyConsumer)
add_test(NAME PBProtocolOnlyConsumerRun COMMAND PBProtocolOnlyConsumer)
add_test(NAME PBCompressionOnlyConsumerRun COMMAND PBCompressionOnlyConsumer)
add_test(NAME PBModulationOnlyConsumerRun COMMAND PBModulationOnlyConsumer)
add_test(NAME PBOuterFecOnlyConsumerRun COMMAND PBOuterFecOnlyConsumer)
add_test(NAME PBReceiverOnlyConsumerRun COMMAND PBReceiverOnlyConsumer)
