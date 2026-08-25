add_library(PBGoldenVector STATIC)
add_library(PB::PBGoldenVector ALIAS PBGoldenVector)

target_sources(PBGoldenVector
    PRIVATE
        src/golden_vector_source.cpp
        src/golden_vector_registry.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/pbgolden/golden_vector_source.h
            include/pbgolden/golden_vector_registry.h)

target_include_directories(PBGoldenVector
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(PBGoldenVector
    PUBLIC
        cxx_std_20)

# The golden byte source is deterministic recomputation support. Independent
# test-only protocol/interleave/raster oracles live in the corresponding test
# suites; these production codec dependencies are implementation details and
# never leak into the public interface.
target_link_libraries(PBGoldenVector
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol
        PB::PBInnerFec
        PB::PBModulation
        PB::PBInterleave)
