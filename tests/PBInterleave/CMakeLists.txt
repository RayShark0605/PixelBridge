# PBInterleave unit tests: full-table bijection across all 16 phases,
# phase/period boundaries, directed nibble/tile patterns, region content
# matrix, span-size contracts, overlap rejection and the checked-arithmetic
# proofs of the frozen permutation constants.
PbAddUnitTest(PBInterleaveTests
    test_interleave.cpp)
target_link_libraries(PBInterleaveTests
    PRIVATE
        PB::PBInterleave
        PB::PBModulation
        PB::PBProtocol)
