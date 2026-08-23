if(NOT DEFINED PB_MANIFEST_PATH OR NOT EXISTS "${PB_MANIFEST_PATH}")
    message(FATAL_ERROR "PB_MANIFEST_PATH must name the PixelBridge manifest")
endif()

file(READ "${PB_MANIFEST_PATH}" manifestJson)

string(JSON baseDependencyCount LENGTH "${manifestJson}" dependencies)
set(blake3DependencyCount 0)
set(zstdDependencyCount 0)
set(wirehairDependencyCount 0)
if(baseDependencyCount GREATER 0)
    math(EXPR lastBaseDependencyIndex "${baseDependencyCount} - 1")
    foreach(dependencyIndex RANGE 0 ${lastBaseDependencyIndex})
        string(JSON dependencyType TYPE
            "${manifestJson}" dependencies ${dependencyIndex})
        if(dependencyType STREQUAL "STRING")
            string(JSON dependencyName GET
                "${manifestJson}" dependencies ${dependencyIndex})
        else()
            string(JSON dependencyName GET
                "${manifestJson}" dependencies ${dependencyIndex} name)
        endif()

        if(dependencyName STREQUAL "catch2")
            message(FATAL_ERROR
                "Catch2 must not be an unconditional manifest dependency")
        endif()
        if(dependencyName STREQUAL "blake3")
            math(EXPR blake3DependencyCount
                "${blake3DependencyCount} + 1")
        endif()
        if(dependencyName STREQUAL "zstd")
            math(EXPR zstdDependencyCount
                "${zstdDependencyCount} + 1")
        endif()
        if(dependencyName STREQUAL "wirehair")
            math(EXPR wirehairDependencyCount
                "${wirehairDependencyCount} + 1")
        endif()
    endforeach()
endif()

if(NOT blake3DependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The base manifest must contain exactly one BLAKE3 dependency")
endif()

if(NOT zstdDependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The base manifest must contain exactly one ZSTD dependency")
endif()

if(NOT wirehairDependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The base manifest must contain exactly one Wirehair dependency")
endif()

string(JSON testDependencyCount LENGTH
    "${manifestJson}" features tests dependencies)
if(testDependencyCount LESS 1)
    message(FATAL_ERROR "The tests feature must depend on Catch2")
endif()

set(catch2DependencyCount 0)
math(EXPR lastTestDependencyIndex "${testDependencyCount} - 1")
foreach(dependencyIndex RANGE 0 ${lastTestDependencyIndex})
    string(JSON dependencyName GET
        "${manifestJson}" features tests dependencies ${dependencyIndex} name)
    if(dependencyName STREQUAL "catch2")
        math(EXPR catch2DependencyCount "${catch2DependencyCount} + 1")
    endif()
endforeach()

if(NOT catch2DependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The tests feature must contain exactly one Catch2 dependency")
endif()

if(NOT DEFINED PB_VCPKG_CONFIGURATION_PATH
        OR NOT EXISTS "${PB_VCPKG_CONFIGURATION_PATH}")
    message(FATAL_ERROR
        "PB_VCPKG_CONFIGURATION_PATH must name vcpkg-configuration.json")
endif()
file(READ "${PB_VCPKG_CONFIGURATION_PATH}" configurationJson)
string(JSON overlayPortCount LENGTH "${configurationJson}" overlay-ports)
set(wirehairOverlayFound OFF)
if(overlayPortCount GREATER 0)
    math(EXPR lastOverlayPortIndex "${overlayPortCount} - 1")
    foreach(overlayPortIndex RANGE 0 ${lastOverlayPortIndex})
        string(JSON overlayPort GET
            "${configurationJson}" overlay-ports ${overlayPortIndex})
        string(REPLACE "\\" "/" normalizedOverlayPort "${overlayPort}")
        if(normalizedOverlayPort STREQUAL "third_party/vcpkg-ports")
            set(wirehairOverlayFound ON)
        endif()
    endforeach()
endif()
if(NOT wirehairOverlayFound)
    message(FATAL_ERROR
        "vcpkg-configuration.json must register third_party/vcpkg-ports")
endif()

if(NOT DEFINED PB_WIREHAIR_PORT_PATH
        OR NOT EXISTS "${PB_WIREHAIR_PORT_PATH}/portfile.cmake"
        OR NOT EXISTS "${PB_WIREHAIR_PORT_PATH}/vcpkg.json")
    message(FATAL_ERROR
        "PB_WIREHAIR_PORT_PATH must name the repository Wirehair overlay")
endif()

file(READ "${PB_WIREHAIR_PORT_PATH}/portfile.cmake" wirehairPortfile)
foreach(requiredPortFragment IN ITEMS
        "REF 067ca7cdb66aed424ec23f97557429bf791c6f0c"
        "SHA512 dc0267b3f441df6b417bb4ecb80d8adbbbc1e44fe235a4f00b9856f2350f9133d3ca325967064b6ecb7bbb35b181ce20a9685f99846d98e74ab86d93a5c3a07a"
        "set(VCPKG_LIBRARY_LINKAGE static)"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DBUILD_TESTS=OFF"
        "-DBUILD_CODEC_V2=OFF"
        "-DMARCH_NATIVE=OFF"
        "-DWIREHAIR_BUILD_BOTH=OFF"
        "-DWIREHAIR_BUILD_TOOLS=OFF"
        "-DWIREHAIR_BUILD_BENCHMARKS=OFF"
        "-DWIREHAIR_ENABLE_SCHEDULED_TESTS=OFF"
        "-DWIREHAIR_ENABLE_LIBFUZZER=OFF"
        "-DWH_LTO=OFF"
        "-DWH_PGO_MODE=OFF")
    string(FIND "${wirehairPortfile}" "${requiredPortFragment}"
        requiredPortFragmentIndex)
    if(requiredPortFragmentIndex EQUAL -1)
        message(FATAL_ERROR
            "Wirehair port is missing pinned option: ${requiredPortFragment}")
    endif()
endforeach()

file(READ "${PB_WIREHAIR_PORT_PATH}/vcpkg.json" wirehairPortManifest)
string(JSON wirehairPortName GET "${wirehairPortManifest}" name)
string(JSON wirehairPortVersion GET "${wirehairPortManifest}" version)
string(JSON wirehairPortLicense GET "${wirehairPortManifest}" license)
if(NOT wirehairPortName STREQUAL "wirehair"
        OR NOT wirehairPortVersion STREQUAL "2.0.0"
        OR NOT wirehairPortLicense STREQUAL "BSD-3-Clause")
    message(FATAL_ERROR
        "Wirehair overlay metadata must remain wirehair 2.0.0 BSD-3-Clause")
endif()
