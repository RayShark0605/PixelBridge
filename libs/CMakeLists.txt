add_subdirectory(PBCore)
add_subdirectory(PBProtocol)
add_subdirectory(PBCompression)
add_subdirectory(PBInnerFec)
add_subdirectory(PBInterleave)
add_subdirectory(PBModulation)
add_subdirectory(PBOuterFec)
add_subdirectory(PBReceiver)

# Golden generation is test/tool/fuzz support, not a production core
# dependency. Keep the source alongside the reusable libraries while avoiding
# the target entirely in apps-only and core-only configurations.
if(PB_BUILD_TOOLS OR PB_BUILD_FUZZERS OR
        (BUILD_TESTING AND PB_BUILD_TESTS))
    add_subdirectory(PBGoldenVector)
endif()
