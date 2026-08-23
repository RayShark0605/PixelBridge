# PixelBridge release baseline. The source revision is part of the dependency
# record, not a PixelBridge wire-protocol version.
set(VCPKG_LIBRARY_LINKAGE static)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO catid/wirehair
    REF 067ca7cdb66aed424ec23f97557429bf791c6f0c
    SHA512 dc0267b3f441df6b417bb4ecb80d8adbbbc1e44fe235a4f00b9856f2350f9133d3ca325967064b6ecb7bbb35b181ce20a9685f99846d98e74ab86d93a5c3a07a
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_CODEC_V2=OFF
        -DMARCH_NATIVE=OFF
        -DWIREHAIR_BUILD_BOTH=OFF
        -DWIREHAIR_BUILD_TOOLS=OFF
        -DWIREHAIR_BUILD_BENCHMARKS=OFF
        -DWIREHAIR_ENABLE_SCHEDULED_TESTS=OFF
        -DWIREHAIR_ENABLE_LIBFUZZER=OFF
        -DWIREHAIR_STRICT_WARNINGS=OFF
        -DWIREHAIR_STATIC_PIC=ON
        -DWH_LTO=OFF
        -DWH_PGO_MODE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME wirehair CONFIG_PATH lib/cmake/wirehair)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

# Preserve the exact canonical profile specification beside the package
# metadata even though upstream also installs a documentation copy.
file(INSTALL
    "${SOURCE_PATH}/V2_WIRE_PROFILE.md"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/pixelbridge-baseline.txt"
    "version=2.0.0\nrevision=067ca7cdb66aed424ec23f97557429bf791c6f0c\n")
