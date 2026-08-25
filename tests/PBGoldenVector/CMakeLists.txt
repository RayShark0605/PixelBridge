# PBGoldenVector cross-suite consistency tests: the deterministic golden
# byte source must reproduce (a) the committed tests/golden/ vectors and
# (b) the inline golden arrays already pinned by the PBProtocol /
# PBModulation / PBInnerFec suites. A disagreement in either direction is a
# regression (golden drift), never a resync opportunity.
PbAddUnitTest(PBGoldenVectorTests
    test_pbgolden_vector.cpp)
target_link_libraries(PBGoldenVectorTests
    PRIVATE
        PB::PBGoldenVector
        PB::PBInterleave
        PB::PBInnerFec
        PB::PBModulation
        PB::PBProtocol)
target_include_directories(PBGoldenVectorTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/PBModulation"
        "${PROJECT_SOURCE_DIR}/tests/PBProtocol")
