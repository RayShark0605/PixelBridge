#pragma once
// ---------------------------------------------------------------------------
// Embedded DVB-S2 Short Frame (N=16200) QC-LDPC matrix tables.
//
// Provenance (third-party data baseline, recorded per design document 39.1):
//   source repo   aff3ct/aff3ct (BSD license), branch develop
//   source commit e8a65c5047262d97a15563b9edc961f69b2792cc
//   source file   include/Tools/Code/LDPC/Standard/DVBS2/DVBS2_constants_16200.hpp
//   raw file SHA256 AF378CA17CA2F400B5ECECEC81BEC69BAE1BCD83788E0856D9C8BC106406F2C3
//   upstream standard ETSI EN 302 307-1 V1.4.1, Short FECFRAME Table 5b.
//
// Cross-checks (all value-for-value, zero delta on every value):
//   1. independent open-source transcription of Table 5b
//      (freecores/dvb_s2_ldpc_decoder, mti/dvbs2_hdef.txt labels
//      2_3s / 11_15s / 37_45s) at extraction time;
//   2. a fresh fetch of the pinned upstream file re-verified against both
//      the kDvbS2ShortAff3ctFlat_* fixtures and the re-flattened
//      structured tables (150/141/158 values per profile).
// The kDvbS2ShortAff3ctFlat_* arrays are the upstream EncValues verbatim and
// are used by the test suite as a transcription guard; they are never
// serialized.
//
// Structure per profile (N = 16200, M = 360 column groups per line):
//   line y in [0, numLines), within-line index l in [0, 360):
//     information bit i = y*360 + l connects to check rows
//       (shift + l*qShift) mod (N-K) for each of the line's shifts.
//   Parity bit j in [0, N-K) connects check rows j and (j+1, if j+1 < N-K).
// All internal tables are private to the library (src-only header); the
// public API exposes profile identity and codec behavior only.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>

namespace pbinnerfec {

inline constexpr std::uint32_t kDvbS2ShortMaxLineDegree = 16;
inline constexpr std::uint32_t kDvbS2ShortMaxLines = 37;
inline constexpr std::uint32_t kDvbS2ShortMaxShiftCount = 121;
inline constexpr std::uint32_t kDvbS2ShortMaxParityBits = 5400;

struct DvbS2ShortMatrix
{
    std::uint32_t kBits;
    std::uint32_t nBits;
    std::uint32_t parityBits;
    std::uint32_t mGroups;
    std::uint32_t qShift;
    std::uint32_t numLines;
    const std::uint8_t* lineDegrees;
    const std::uint16_t* lineShifts;
    const std::uint16_t* lineShiftOffsets;
};



// Robust (K=10800, N-K=5400, Q=15, lines=30): upstream struct dvbs2_values_16200_5400
inline constexpr std::array<std::uint8_t, 30> kDvbS2ShortLineDegrees_Robust = {
    13,13,13,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3
};
inline constexpr std::array<std::uint16_t, 120> kDvbS2ShortLineShifts_Robust = {
    0,2084,1613,1548,1286,1460,3196,4297,2481,3369,3451,4620,2622,1,122,1516,
    3448,2880,1407,1847,3799,3529,373,971,4358,3108,2,259,3399,929,2650,864,
    3996,3833,107,5287,164,3125,2350,3,342,3529,4,4198,2147,5,1880,4836,
    6,3864,4910,7,243,1542,8,3011,1436,9,2167,2512,10,4606,1003,11,
    2835,705,12,3426,2365,13,3848,2474,14,1360,1743,0,163,2536,1,2583,
    1180,2,1542,509,3,4418,1005,4,5212,5117,5,2155,2922,6,347,2696,
    7,226,4296,8,1560,487,9,3926,1640,10,149,2928,11,2364,563,12,
    635,688,13,231,1684,14,1129,3894
};
inline constexpr std::array<std::uint16_t, 30> kDvbS2ShortLineShiftOffsets_Robust = {
    0,13,26,39,42,45,48,51,54,57,60,63,66,69,72,75,78,81,84,87,90,93,96,99,102,105,108,111,114,117
};

// Balanced (K=11880, N-K=4320, Q=12, lines=33): upstream struct dvbs2_values_16200_4320
inline constexpr std::array<std::uint8_t, 33> kDvbS2ShortLineDegrees_Balanced = {
    12,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    3,3,3
};
inline constexpr std::array<std::uint16_t, 108> kDvbS2ShortLineShifts_Balanced = {
    3,3198,478,4207,1481,1009,2616,1924,3437,554,683,1801,4,2681,2135,5,
    3107,4027,6,2637,3373,7,3830,3449,8,4129,2060,9,4184,2742,10,3946,
    1070,11,2239,984,0,1458,3031,1,3003,1328,2,1137,1716,3,132,3725,
    4,1817,638,5,1774,3447,6,3632,1257,7,542,3694,8,1015,1945,9,
    1948,412,10,995,2238,11,4141,1907,0,2480,3079,1,3021,1088,2,713,
    1379,3,997,3903,4,2323,3361,5,1110,986,6,2532,142,7,1690,2405,
    8,1298,1881,9,615,174,10,1648,3112,11,1415,2808
};
inline constexpr std::array<std::uint16_t, 33> kDvbS2ShortLineShiftOffsets_Balanced = {
    0,12,15,18,21,24,27,30,33,36,39,42,45,48,51,54,57,60,63,66,69,72,75,78,81,84,87,90,93,96,
    99,102,105
};

// Fast (K=13320, N-K=2880, Q=8, lines=37): upstream struct dvbs2_values_16200_2880
inline constexpr std::array<std::uint8_t, 37> kDvbS2ShortLineDegrees_Fast = {
    13,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3
};
inline constexpr std::array<std::uint16_t, 121> kDvbS2ShortLineShifts_Fast = {
    3,2409,499,1481,908,559,716,1270,333,2508,2264,1702,2805,4,2447,1926,
    5,414,1224,6,2114,842,7,212,573,0,2383,2112,1,2286,2348,2,
    545,819,3,1264,143,4,1701,2258,5,964,166,6,114,2413,7,2243,
    81,0,1245,1581,1,775,169,2,1696,1104,3,1914,2831,4,532,1450,
    5,91,974,6,497,2228,7,2326,1579,0,2482,256,1,1117,1261,2,
    1257,1658,3,1478,1225,4,2511,980,5,2320,2675,6,435,1278,7,228,
    503,0,1885,2369,1,57,483,2,838,1050,3,1231,1990,4,1738,68,
    5,2392,951,6,163,645,7,2644,1704
};
inline constexpr std::array<std::uint16_t, 37> kDvbS2ShortLineShiftOffsets_Fast = {
    0,13,16,19,22,25,28,31,34,37,40,43,46,49,52,55,58,61,64,67,70,73,76,79,82,85,88,91,94,97,
    100,103,106,109,112,115,118
};

// Upstream EncValues verbatim (degree-prefixed flat layout); test fixture only.
inline constexpr std::array<std::int32_t, 150> kDvbS2ShortAff3ctFlat_Robust = {
    13,0,2084,1613,1548,1286,1460,3196,4297,2481,
    3369,3451,4620,2622,13,1,122,1516,3448,2880,
    1407,1847,3799,3529,373,971,4358,3108,13,2,
    259,3399,929,2650,864,3996,3833,107,5287,164,
    3125,2350,3,3,342,3529,3,4,4198,2147,
    3,5,1880,4836,3,6,3864,4910,3,7,
    243,1542,3,8,3011,1436,3,9,2167,2512,
    3,10,4606,1003,3,11,2835,705,3,12,
    3426,2365,3,13,3848,2474,3,14,1360,1743,
    3,0,163,2536,3,1,2583,1180,3,2,
    1542,509,3,3,4418,1005,3,4,5212,5117,
    3,5,2155,2922,3,6,347,2696,3,7,
    226,4296,3,8,1560,487,3,9,3926,1640,
    3,10,149,2928,3,11,2364,563,3,12,
    635,688,3,13,231,1684,3,14,1129,3894
};
inline constexpr std::array<std::int32_t, 141> kDvbS2ShortAff3ctFlat_Balanced = {
    12,3,3198,478,4207,1481,1009,2616,1924,3437,
    554,683,1801,3,4,2681,2135,3,5,3107,
    4027,3,6,2637,3373,3,7,3830,3449,3,
    8,4129,2060,3,9,4184,2742,3,10,3946,
    1070,3,11,2239,984,3,0,1458,3031,3,
    1,3003,1328,3,2,1137,1716,3,3,132,
    3725,3,4,1817,638,3,5,1774,3447,3,
    6,3632,1257,3,7,542,3694,3,8,1015,
    1945,3,9,1948,412,3,10,995,2238,3,
    11,4141,1907,3,0,2480,3079,3,1,3021,
    1088,3,2,713,1379,3,3,997,3903,3,
    4,2323,3361,3,5,1110,986,3,6,2532,
    142,3,7,1690,2405,3,8,1298,1881,3,
    9,615,174,3,10,1648,3112,3,11,1415,
    2808
};
inline constexpr std::array<std::int32_t, 158> kDvbS2ShortAff3ctFlat_Fast = {
    13,3,2409,499,1481,908,559,716,1270,333,
    2508,2264,1702,2805,3,4,2447,1926,3,5,
    414,1224,3,6,2114,842,3,7,212,573,
    3,0,2383,2112,3,1,2286,2348,3,2,
    545,819,3,3,1264,143,3,4,1701,2258,
    3,5,964,166,3,6,114,2413,3,7,
    2243,81,3,0,1245,1581,3,1,775,169,
    3,2,1696,1104,3,3,1914,2831,3,4,
    532,1450,3,5,91,974,3,6,497,2228,
    3,7,2326,1579,3,0,2482,256,3,1,
    1117,1261,3,2,1257,1658,3,3,1478,1225,
    3,4,2511,980,3,5,2320,2675,3,6,
    435,1278,3,7,228,503,3,0,1885,2369,
    3,1,57,483,3,2,838,1050,3,3,
    1231,1990,3,4,1738,68,3,5,2392,951,
    3,6,163,645,3,7,2644,1704
};

// Frozen per-profile matrix views. inline constexpr keeps one ODR entity
// per translation unit set; the row pointers reference the library-internal
// arrays above and are stable for the process lifetime.
inline constexpr const DvbS2ShortMatrix kDvbS2ShortMatrix_Robust = {
    10800, 16200, 5400, 360, 15, 30,
    kDvbS2ShortLineDegrees_Robust.data(),
    kDvbS2ShortLineShifts_Robust.data(),
    kDvbS2ShortLineShiftOffsets_Robust.data()};
inline constexpr const DvbS2ShortMatrix kDvbS2ShortMatrix_Balanced = {
    11880, 16200, 4320, 360, 12, 33,
    kDvbS2ShortLineDegrees_Balanced.data(),
    kDvbS2ShortLineShifts_Balanced.data(),
    kDvbS2ShortLineShiftOffsets_Balanced.data()};
inline constexpr const DvbS2ShortMatrix kDvbS2ShortMatrix_Fast = {
    13320, 16200, 2880, 360, 8, 37,
    kDvbS2ShortLineDegrees_Fast.data(),
    kDvbS2ShortLineShifts_Fast.data(),
    kDvbS2ShortLineShiftOffsets_Fast.data()};

// Resolves the embedded table for an exact short-frame K. Returns nullptr
// for every K outside the frozen set (fail-closed).
[[nodiscard]] inline const DvbS2ShortMatrix* GetDvbS2ShortMatrix(
    const std::uint32_t kBits) noexcept
{
    switch (kBits)
    {
        case 10800:
            return &kDvbS2ShortMatrix_Robust;
        case 11880:
            return &kDvbS2ShortMatrix_Balanced;
        case 13320:
            return &kDvbS2ShortMatrix_Fast;
        default:
            return nullptr;
    }
}

} // namespace pbinnerfec
