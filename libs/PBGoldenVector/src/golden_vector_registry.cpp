#include "pbgolden/golden_vector_registry.h"

#include "pbgolden/golden_vector_source.h"

#include <array>
#include <exception>

namespace pbgolden {

namespace {

// ---------------------------------------------------------------------------
// Pinned BLAKE3-256 digests (canonical vector bytes).
//
// These values are frozen: they are generated once by the deterministic byte
// source, cross-validated against the existing test-suite pins, and pinned
// here and in docs/GOLDEN_VECTOR_HARNESS.md. Regeneration is a deliberate,
// reviewed act (see the regeneration flow in the document); a digest drift
// during a normal build is a regression signal, never a resync opportunity.
// ---------------------------------------------------------------------------

inline constexpr std::array<std::byte, 32> kBootstrapRecordBlake3{
    std::byte{0x00}, std::byte{0x43}, std::byte{0x37}, std::byte{0xce}, std::byte{0x10}, std::byte{0x21}, std::byte{0x48}, std::byte{0x14},

    std::byte{0x69}, std::byte{0x35}, std::byte{0xe6}, std::byte{0x7d}, std::byte{0x2c}, std::byte{0x56}, std::byte{0xda}, std::byte{0x6c},

    std::byte{0x48}, std::byte{0x12}, std::byte{0x2f}, std::byte{0xb0}, std::byte{0x93}, std::byte{0x7e}, std::byte{0xce}, std::byte{0x94},

    std::byte{0x24}, std::byte{0x4c}, std::byte{0xeb}, std::byte{0xae}, std::byte{0x86}, std::byte{0xb6}, std::byte{0x23}, std::byte{0xc2}
};
inline constexpr std::array<std::byte, 32> kControlSessionDescriptorBlake3{
    std::byte{0x5b}, std::byte{0x80}, std::byte{0x6b}, std::byte{0x36}, std::byte{0x05}, std::byte{0x9f}, std::byte{0xd5}, std::byte{0x26},

    std::byte{0x2a}, std::byte{0xfc}, std::byte{0x8f}, std::byte{0x74}, std::byte{0x6e}, std::byte{0x01}, std::byte{0x90}, std::byte{0x84},

    std::byte{0xb8}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xe3}, std::byte{0x5b}, std::byte{0x77}, std::byte{0xd8}, std::byte{0xe5},

    std::byte{0x03}, std::byte{0x98}, std::byte{0x38}, std::byte{0xb0}, std::byte{0x22}, std::byte{0x46}, std::byte{0x62}, std::byte{0xe0}};
inline constexpr std::array<std::byte, 32> kControlEmptyBlake3{
    std::byte{0x07}, std::byte{0xa3}, std::byte{0xb9}, std::byte{0x20}, std::byte{0xf3}, std::byte{0x6d}, std::byte{0xdd}, std::byte{0x17},

    std::byte{0x52}, std::byte{0xf7}, std::byte{0x93}, std::byte{0xf4}, std::byte{0x73}, std::byte{0x2d}, std::byte{0xa4}, std::byte{0x1f},

    std::byte{0x51}, std::byte{0xee}, std::byte{0xe0}, std::byte{0x48}, std::byte{0xb6}, std::byte{0x2e}, std::byte{0x83}, std::byte{0x2f},

    std::byte{0x67}, std::byte{0xfc}, std::byte{0x4d}, std::byte{0xc3}, std::byte{0x30}, std::byte{0x30}, std::byte{0x63}, std::byte{0xbc}
};
inline constexpr std::array<std::byte, 32> kControlMaximumBlake3{
    std::byte{0x7b}, std::byte{0x2d}, std::byte{0xd1}, std::byte{0x58}, std::byte{0x08}, std::byte{0x40}, std::byte{0xdf}, std::byte{0xc8},

    std::byte{0xaf}, std::byte{0x29}, std::byte{0x55}, std::byte{0xed}, std::byte{0x65}, std::byte{0x4e}, std::byte{0x66}, std::byte{0xb7},

    std::byte{0xd5}, std::byte{0x50}, std::byte{0x9b}, std::byte{0x31}, std::byte{0xc7}, std::byte{0x14}, std::byte{0xa9}, std::byte{0xc7},

    std::byte{0x31}, std::byte{0x51}, std::byte{0x3d}, std::byte{0x56}, std::byte{0x05}, std::byte{0x4b}, std::byte{0x86}, std::byte{0x82}
};
inline constexpr std::array<std::byte, 32> kControlFragment0Blake3{
    std::byte{0x2f}, std::byte{0x0d}, std::byte{0x4e}, std::byte{0x8a}, std::byte{0xbd}, std::byte{0xfa}, std::byte{0x2b}, std::byte{0xaf},

    std::byte{0xee}, std::byte{0x45}, std::byte{0x69}, std::byte{0x91}, std::byte{0xd5}, std::byte{0x58}, std::byte{0x26}, std::byte{0xe4},

    std::byte{0xf8}, std::byte{0x8c}, std::byte{0xd2}, std::byte{0x46}, std::byte{0x38}, std::byte{0x61}, std::byte{0xf5}, std::byte{0xe3},

    std::byte{0x86}, std::byte{0xe4}, std::byte{0x22}, std::byte{0x9e}, std::byte{0xe4}, std::byte{0x5a}, std::byte{0x43}, std::byte{0xab}
};
inline constexpr std::array<std::byte, 32> kControlFragment1Blake3{
    std::byte{0x8f}, std::byte{0x80}, std::byte{0xd5}, std::byte{0xe0}, std::byte{0xe6}, std::byte{0xf1}, std::byte{0x4a}, std::byte{0x28},

    std::byte{0x6b}, std::byte{0xa2}, std::byte{0x4d}, std::byte{0x62}, std::byte{0xfc}, std::byte{0x36}, std::byte{0xee}, std::byte{0x8f},

    std::byte{0xa2}, std::byte{0x76}, std::byte{0xd5}, std::byte{0x0a}, std::byte{0x9e}, std::byte{0x4e}, std::byte{0x0b}, std::byte{0xbe},

    std::byte{0x4a}, std::byte{0xed}, std::byte{0x12}, std::byte{0x01}, std::byte{0x5c}, std::byte{0xef}, std::byte{0x19}, std::byte{0x07}};
inline constexpr std::array<std::byte, 32> kControlFragment2Blake3{
    std::byte{0x90}, std::byte{0x43}, std::byte{0x2b}, std::byte{0x43}, std::byte{0xbd}, std::byte{0x9c}, std::byte{0x6a}, std::byte{0xb7},

    std::byte{0x2c}, std::byte{0x7e}, std::byte{0x35}, std::byte{0x1c}, std::byte{0x53}, std::byte{0x54}, std::byte{0x5d}, std::byte{0x39},

    std::byte{0xf0}, std::byte{0x22}, std::byte{0x08}, std::byte{0x99}, std::byte{0xcb}, std::byte{0xf7}, std::byte{0xae}, std::byte{0xe3},

    std::byte{0xe0}, std::byte{0x98}, std::byte{0xdd}, std::byte{0xf3}, std::byte{0x58}, std::byte{0x78}, std::byte{0x64}, std::byte{0x88}};
inline constexpr std::array<std::byte, 32> kSessionDescriptorBlake3{
    std::byte{0x1a}, std::byte{0x45}, std::byte{0x3a}, std::byte{0x5d}, std::byte{0x87}, std::byte{0xe3}, std::byte{0xa1}, std::byte{0x90},

    std::byte{0xbf}, std::byte{0x23}, std::byte{0xee}, std::byte{0x15}, std::byte{0x54}, std::byte{0x60}, std::byte{0xfe}, std::byte{0x33},

    std::byte{0xf1}, std::byte{0x4e}, std::byte{0x09}, std::byte{0x9c}, std::byte{0xe1}, std::byte{0x94}, std::byte{0x4a}, std::byte{0x16},

    std::byte{0x18}, std::byte{0xbe}, std::byte{0x3a}, std::byte{0xb8}, std::byte{0x8e}, std::byte{0xad}, std::byte{0x33}, std::byte{0x8f}};
inline constexpr std::array<std::byte, 32> kSegmentDescriptorDirectRepeatBlake3{
    std::byte{0x80}, std::byte{0xd8}, std::byte{0xab}, std::byte{0x82}, std::byte{0xb9}, std::byte{0x21}, std::byte{0x26}, std::byte{0xfd},

    std::byte{0xd2}, std::byte{0xda}, std::byte{0x49}, std::byte{0x3a}, std::byte{0x7b}, std::byte{0x05}, std::byte{0x64}, std::byte{0x2a},

    std::byte{0x7c}, std::byte{0x8e}, std::byte{0x5c}, std::byte{0xdb}, std::byte{0xbe}, std::byte{0x2f}, std::byte{0x95}, std::byte{0x4e},

    std::byte{0x23}, std::byte{0x85}, std::byte{0x36}, std::byte{0x88}, std::byte{0xfe}, std::byte{0x49}, std::byte{0x03}, std::byte{0xd3}};
inline constexpr std::array<std::byte, 32> kSegmentDescriptorWirehairBlake3{
    std::byte{0x00}, std::byte{0xf4}, std::byte{0x92}, std::byte{0x16}, std::byte{0xd0}, std::byte{0xe7}, std::byte{0x4b}, std::byte{0x01},

    std::byte{0xdd}, std::byte{0xa5}, std::byte{0xbd}, std::byte{0x64}, std::byte{0x5d}, std::byte{0x09}, std::byte{0x8f}, std::byte{0x98},

    std::byte{0x73}, std::byte{0xb8}, std::byte{0xe0}, std::byte{0x61}, std::byte{0x51}, std::byte{0x1c}, std::byte{0x51}, std::byte{0x43},

    std::byte{0xa5}, std::byte{0x0e}, std::byte{0x8f}, std::byte{0xbd}, std::byte{0x98}, std::byte{0x18}, std::byte{0xbb}, std::byte{0x41}};
inline constexpr std::array<std::byte, 32> kFinalManifestBlake3{
    std::byte{0xff}, std::byte{0x08}, std::byte{0x35}, std::byte{0xcb}, std::byte{0x71}, std::byte{0xe9}, std::byte{0x9a}, std::byte{0xc0},

    std::byte{0x21}, std::byte{0xf7}, std::byte{0xf8}, std::byte{0xf1}, std::byte{0x30}, std::byte{0x70}, std::byte{0xd7}, std::byte{0x39},

    std::byte{0x8d}, std::byte{0x4c}, std::byte{0x1c}, std::byte{0x7a}, std::byte{0xc3}, std::byte{0xbf}, std::byte{0x59}, std::byte{0xa8},

    std::byte{0x58}, std::byte{0x17}, std::byte{0x98}, std::byte{0x91}, std::byte{0x88}, std::byte{0x8f}, std::byte{0x3b}, std::byte{0xf9}};
inline constexpr std::array<std::byte, 32> kWirehairCanonicalDescriptorBlake3{
    std::byte{0xb5}, std::byte{0x8f}, std::byte{0x86}, std::byte{0x8f}, std::byte{0x5d}, std::byte{0x3c}, std::byte{0x46}, std::byte{0xf0},

    std::byte{0x33}, std::byte{0x9b}, std::byte{0x4e}, std::byte{0x5f}, std::byte{0x97}, std::byte{0xfb}, std::byte{0x4b}, std::byte{0x95},

    std::byte{0x90}, std::byte{0x69}, std::byte{0xfc}, std::byte{0xb3}, std::byte{0x09}, std::byte{0xf3}, std::byte{0x20}, std::byte{0xa5},

    std::byte{0x97}, std::byte{0x89}, std::byte{0xb2}, std::byte{0xf1}, std::byte{0x64}, std::byte{0xd5}, std::byte{0x11}, std::byte{0xf3}
};
inline constexpr std::array<std::byte, 32> kTransportBlockMinBlake3{
    std::byte{0xa8}, std::byte{0x72}, std::byte{0x74}, std::byte{0xcb}, std::byte{0xf3}, std::byte{0xdc}, std::byte{0x54}, std::byte{0xef},

    std::byte{0xcd}, std::byte{0x97}, std::byte{0x79}, std::byte{0x66}, std::byte{0x34}, std::byte{0xe1}, std::byte{0xd9}, std::byte{0xa2},

    std::byte{0xd4}, std::byte{0xf7}, std::byte{0xb1}, std::byte{0x7d}, std::byte{0x3e}, std::byte{0x29}, std::byte{0x23}, std::byte{0xcd},

    std::byte{0xe5}, std::byte{0x85}, std::byte{0xa6}, std::byte{0x4d}, std::byte{0xb6}, std::byte{0x4c}, std::byte{0x73}, std::byte{0x86}
};
inline constexpr std::array<std::byte, 32> kTransportBlockCanonicalBlake3{
    std::byte{0x3a}, std::byte{0x11}, std::byte{0xbb}, std::byte{0x4a}, std::byte{0x55}, std::byte{0x45}, std::byte{0x33}, std::byte{0xa5},

    std::byte{0x39}, std::byte{0x33}, std::byte{0x0d}, std::byte{0x0b}, std::byte{0x30}, std::byte{0xec}, std::byte{0x43}, std::byte{0x4c},

    std::byte{0x4d}, std::byte{0x57}, std::byte{0x40}, std::byte{0x04}, std::byte{0x07}, std::byte{0x49}, std::byte{0xe0}, std::byte{0x4d},

    std::byte{0x1f}, std::byte{0x45}, std::byte{0x86}, std::byte{0x3d}, std::byte{0x82}, std::byte{0x73}, std::byte{0x31}, std::byte{0xc1}
};
inline constexpr std::array<std::byte, 32> kTransportBlockMaxPayloadBlake3{
    std::byte{0xff}, std::byte{0xbc}, std::byte{0xac}, std::byte{0x9c}, std::byte{0xb3}, std::byte{0x26}, std::byte{0x6f}, std::byte{0x3e},

    std::byte{0xa6}, std::byte{0xf0}, std::byte{0x02}, std::byte{0x7e}, std::byte{0x56}, std::byte{0xe8}, std::byte{0x7e}, std::byte{0xaf},

    std::byte{0x09}, std::byte{0xbc}, std::byte{0x07}, std::byte{0xe6}, std::byte{0x0e}, std::byte{0xae}, std::byte{0xb9}, std::byte{0x39},

    std::byte{0xf5}, std::byte{0xcc}, std::byte{0xf5}, std::byte{0x9c}, std::byte{0x26}, std::byte{0x5a}, std::byte{0xd3}, std::byte{0x3e}
};
inline constexpr std::array<std::byte, 32> kCodewordRobustBlake3{
    std::byte{0x84}, std::byte{0xd4}, std::byte{0x3e}, std::byte{0x6e}, std::byte{0x4f}, std::byte{0xb7}, std::byte{0x48}, std::byte{0xdf},

    std::byte{0x1d}, std::byte{0xe5}, std::byte{0xf9}, std::byte{0x03}, std::byte{0xf1}, std::byte{0xf2}, std::byte{0xbd}, std::byte{0x77},

    std::byte{0xd6}, std::byte{0x72}, std::byte{0x00}, std::byte{0x17}, std::byte{0x84}, std::byte{0xb4}, std::byte{0x02}, std::byte{0x54},

    std::byte{0x78}, std::byte{0x8a}, std::byte{0x8e}, std::byte{0x93}, std::byte{0x20}, std::byte{0x91}, std::byte{0x17}, std::byte{0xa9}
};
inline constexpr std::array<std::byte, 32> kCodewordBalancedBlake3{
    std::byte{0x51}, std::byte{0x5e}, std::byte{0xde}, std::byte{0xa2}, std::byte{0x06}, std::byte{0x42}, std::byte{0xff}, std::byte{0x96},

    std::byte{0xd7}, std::byte{0xec}, std::byte{0xe9}, std::byte{0xdb}, std::byte{0xca}, std::byte{0xf8}, std::byte{0x77}, std::byte{0xea},

    std::byte{0x13}, std::byte{0x58}, std::byte{0x26}, std::byte{0xad}, std::byte{0x90}, std::byte{0x43}, std::byte{0x73}, std::byte{0x6f},

    std::byte{0xa4}, std::byte{0x6a}, std::byte{0x61}, std::byte{0xce}, std::byte{0x78}, std::byte{0x71}, std::byte{0x45}, std::byte{0x94}};
inline constexpr std::array<std::byte, 32> kCodewordFastBlake3{
    std::byte{0xbf}, std::byte{0x42}, std::byte{0x8f}, std::byte{0x59}, std::byte{0xe2}, std::byte{0x6c}, std::byte{0x7e}, std::byte{0x71},

    std::byte{0x5f}, std::byte{0x79}, std::byte{0xae}, std::byte{0x2f}, std::byte{0xae}, std::byte{0x19}, std::byte{0x13}, std::byte{0x04},

    std::byte{0xc8}, std::byte{0x50}, std::byte{0x1b}, std::byte{0x82}, std::byte{0xea}, std::byte{0x2b}, std::byte{0xf1}, std::byte{0xc5},

    std::byte{0x06}, std::byte{0x0b}, std::byte{0x36}, std::byte{0xd1}, std::byte{0x46}, std::byte{0x50}, std::byte{0xa6}, std::byte{0x67}};
inline constexpr std::array<std::byte, 32> kInterleaveMappingSampleBlake3{
    std::byte{0x46}, std::byte{0x9b}, std::byte{0x3f}, std::byte{0xc9}, std::byte{0x26}, std::byte{0x51}, std::byte{0x24}, std::byte{0xf5},

    std::byte{0x8e}, std::byte{0x8d}, std::byte{0x7f}, std::byte{0x7f}, std::byte{0xca}, std::byte{0xef}, std::byte{0x76}, std::byte{0x1e},

    std::byte{0xfc}, std::byte{0x53}, std::byte{0xb4}, std::byte{0x58}, std::byte{0x24}, std::byte{0xc8}, std::byte{0x80}, std::byte{0xc7},

    std::byte{0x24}, std::byte{0x5c}, std::byte{0x3b}, std::byte{0xba}, std::byte{0xaa}, std::byte{0xf7}, std::byte{0xa5}, std::byte{0x54}
};
inline constexpr std::array<std::byte, 32> kInterleaveRegionLogicalBlake3{
    std::byte{0x32}, std::byte{0x17}, std::byte{0xf9}, std::byte{0x93}, std::byte{0xfd}, std::byte{0xea}, std::byte{0xe1}, std::byte{0x26},

    std::byte{0xb6}, std::byte{0x9d}, std::byte{0x49}, std::byte{0xae}, std::byte{0xc2}, std::byte{0xde}, std::byte{0xa9}, std::byte{0x14},

    std::byte{0x4e}, std::byte{0x90}, std::byte{0x1f}, std::byte{0xb7}, std::byte{0x14}, std::byte{0x23}, std::byte{0x98}, std::byte{0x74},

    std::byte{0xc6}, std::byte{0xf0}, std::byte{0xf2}, std::byte{0xfc}, std::byte{0x96}, std::byte{0xd7}, std::byte{0xc3}, std::byte{0x9e}
};
inline constexpr std::array<std::byte, 32> kInterleaveRegionPhysicalPhase7Blake3{
    std::byte{0x1f}, std::byte{0xa9}, std::byte{0x6a}, std::byte{0xe4}, std::byte{0x37}, std::byte{0xc0}, std::byte{0xd3}, std::byte{0xc3},

    std::byte{0x69}, std::byte{0xd6}, std::byte{0x9a}, std::byte{0x2e}, std::byte{0x2c}, std::byte{0x8c}, std::byte{0x2a}, std::byte{0x27},

    std::byte{0xe8}, std::byte{0xc8}, std::byte{0x0c}, std::byte{0x37}, std::byte{0x99}, std::byte{0x5c}, std::byte{0x58}, std::byte{0xda},

    std::byte{0x97}, std::byte{0x1e}, std::byte{0x1a}, std::byte{0xf7}, std::byte{0x6f}, std::byte{0xbd}, std::byte{0xb5}, std::byte{0x79}
};
inline constexpr std::array<std::byte, 32> kG1TransportInfoBlockBlake3{
    std::byte{0x3a}, std::byte{0x11}, std::byte{0xbb}, std::byte{0x4a}, std::byte{0x55}, std::byte{0x45}, std::byte{0x33}, std::byte{0xa5},

    std::byte{0x39}, std::byte{0x33}, std::byte{0x0d}, std::byte{0x0b}, std::byte{0x30}, std::byte{0xec}, std::byte{0x43}, std::byte{0x4c},

    std::byte{0x4d}, std::byte{0x57}, std::byte{0x40}, std::byte{0x04}, std::byte{0x07}, std::byte{0x49}, std::byte{0xe0}, std::byte{0x4d},

    std::byte{0x1f}, std::byte{0x45}, std::byte{0x86}, std::byte{0x3d}, std::byte{0x82}, std::byte{0x73}, std::byte{0x31}, std::byte{0xc1}
};
inline constexpr std::array<std::byte, 32> kG1TransportDecodedBlake3{
    std::byte{0x3a}, std::byte{0x11}, std::byte{0xbb}, std::byte{0x4a}, std::byte{0x55}, std::byte{0x45}, std::byte{0x33}, std::byte{0xa5},

    std::byte{0x39}, std::byte{0x33}, std::byte{0x0d}, std::byte{0x0b}, std::byte{0x30}, std::byte{0xec}, std::byte{0x43}, std::byte{0x4c},

    std::byte{0x4d}, std::byte{0x57}, std::byte{0x40}, std::byte{0x04}, std::byte{0x07}, std::byte{0x49}, std::byte{0xe0}, std::byte{0x4d},

    std::byte{0x1f}, std::byte{0x45}, std::byte{0x86}, std::byte{0x3d}, std::byte{0x82}, std::byte{0x73}, std::byte{0x31}, std::byte{0xc1}
};
inline constexpr std::array<std::byte, 32> kG1Transport2CwInfoBlockBlake3{
    std::byte{0xd0}, std::byte{0xb7}, std::byte{0xc5}, std::byte{0x09}, std::byte{0x34}, std::byte{0xcb}, std::byte{0x12}, std::byte{0x22},

    std::byte{0x7d}, std::byte{0x32}, std::byte{0x46}, std::byte{0xb9}, std::byte{0x32}, std::byte{0x24}, std::byte{0x92}, std::byte{0x61},

    std::byte{0xc9}, std::byte{0x35}, std::byte{0x9c}, std::byte{0x6f}, std::byte{0x52}, std::byte{0xec}, std::byte{0xbd}, std::byte{0x80},

    std::byte{0x3e}, std::byte{0x1b}, std::byte{0x51}, std::byte{0xb6}, std::byte{0xc2}, std::byte{0x05}, std::byte{0x2a}, std::byte{0x4e}
};

// Frame digest pins (raw PBRW primary pin, PNG secondary pin).
inline constexpr std::array<std::byte, 32> kG0ZeroRawBlake3{
    std::byte{0x29}, std::byte{0xed}, std::byte{0x5c}, std::byte{0x87}, std::byte{0x25}, std::byte{0xa5}, std::byte{0xd9}, std::byte{0x39},

    std::byte{0x68}, std::byte{0x52}, std::byte{0x49}, std::byte{0xbe}, std::byte{0x82}, std::byte{0xef}, std::byte{0x5e}, std::byte{0x37},

    std::byte{0x17}, std::byte{0xc5}, std::byte{0xce}, std::byte{0xb3}, std::byte{0x54}, std::byte{0x50}, std::byte{0x01}, std::byte{0xab},

    std::byte{0x0b}, std::byte{0xed}, std::byte{0xdd}, std::byte{0xf7}, std::byte{0xe2}, std::byte{0x50}, std::byte{0x0b}, std::byte{0x0b}
};
inline constexpr std::array<std::byte, 32> kG0ZeroPngBlake3{
    std::byte{0x20}, std::byte{0xea}, std::byte{0x18}, std::byte{0x4a}, std::byte{0xdc}, std::byte{0x00}, std::byte{0xf1}, std::byte{0x64},

    std::byte{0xb1}, std::byte{0xc8}, std::byte{0xfe}, std::byte{0x71}, std::byte{0xb7}, std::byte{0xc5}, std::byte{0x68}, std::byte{0xf2},

    std::byte{0x40}, std::byte{0xb1}, std::byte{0xe8}, std::byte{0x42}, std::byte{0x89}, std::byte{0x91}, std::byte{0xe8}, std::byte{0x67},

    std::byte{0xe0}, std::byte{0xe0}, std::byte{0xb8}, std::byte{0x88}, std::byte{0x29}, std::byte{0x14}, std::byte{0x40}, std::byte{0x02}
};
inline constexpr std::array<std::byte, 32> kG1CanonicalRawBlake3{
    std::byte{0x54}, std::byte{0xc2}, std::byte{0x3b}, std::byte{0x3e}, std::byte{0x55}, std::byte{0x8b}, std::byte{0xf4}, std::byte{0x80},

    std::byte{0x5a}, std::byte{0xfa}, std::byte{0xdb}, std::byte{0x61}, std::byte{0x93}, std::byte{0x21}, std::byte{0x7d}, std::byte{0x9e},

    std::byte{0xf1}, std::byte{0x11}, std::byte{0xbc}, std::byte{0xb5}, std::byte{0x97}, std::byte{0x5e}, std::byte{0xe1}, std::byte{0x33},

    std::byte{0x6a}, std::byte{0x63}, std::byte{0x3a}, std::byte{0xd5}, std::byte{0x69}, std::byte{0xbf}, std::byte{0x0d}, std::byte{0x57}
};
inline constexpr std::array<std::byte, 32> kG1CanonicalPngBlake3{
    std::byte{0xf6}, std::byte{0x59}, std::byte{0x97}, std::byte{0xdf}, std::byte{0x4b}, std::byte{0x61}, std::byte{0x24}, std::byte{0x12},

    std::byte{0xcb}, std::byte{0x2c}, std::byte{0xe5}, std::byte{0x0d}, std::byte{0xe3}, std::byte{0xc0}, std::byte{0xb9}, std::byte{0xf9},

    std::byte{0xfd}, std::byte{0x54}, std::byte{0x2b}, std::byte{0xc8}, std::byte{0x48}, std::byte{0x42}, std::byte{0xb9}, std::byte{0xf0},

    std::byte{0x84}, std::byte{0xb7}, std::byte{0x6c}, std::byte{0x5f}, std::byte{0x6e}, std::byte{0x29}, std::byte{0x0a}, std::byte{0x89}
};
inline constexpr std::array<std::byte, 32> kG1TransportRawBlake3{
    std::byte{0x60}, std::byte{0x05}, std::byte{0x70}, std::byte{0x6a}, std::byte{0xba}, std::byte{0x9d}, std::byte{0x66}, std::byte{0x76},

    std::byte{0xc5}, std::byte{0x95}, std::byte{0x01}, std::byte{0x92}, std::byte{0x4f}, std::byte{0x9b}, std::byte{0xbe}, std::byte{0xae},

    std::byte{0x59}, std::byte{0x2f}, std::byte{0x12}, std::byte{0xe2}, std::byte{0x9a}, std::byte{0x36}, std::byte{0x18}, std::byte{0x4d},

    std::byte{0x02}, std::byte{0xf3}, std::byte{0x4e}, std::byte{0x32}, std::byte{0x9a}, std::byte{0x47}, std::byte{0xea}, std::byte{0x45}
};
inline constexpr std::array<std::byte, 32> kG1TransportPngBlake3{
    std::byte{0xe0}, std::byte{0xbf}, std::byte{0x73}, std::byte{0x36}, std::byte{0x22}, std::byte{0x97}, std::byte{0x7c}, std::byte{0xd5},

    std::byte{0xf0}, std::byte{0x8e}, std::byte{0x36}, std::byte{0xbf}, std::byte{0x2e}, std::byte{0x60}, std::byte{0x76}, std::byte{0x39},

    std::byte{0x45}, std::byte{0x44}, std::byte{0x24}, std::byte{0x99}, std::byte{0xe0}, std::byte{0x49}, std::byte{0x4a}, std::byte{0x0b},

    std::byte{0x4f}, std::byte{0xb2}, std::byte{0xba}, std::byte{0xad}, std::byte{0x05}, std::byte{0x80}, std::byte{0x02}, std::byte{0x0c}
};
inline constexpr std::array<std::byte, 32> kG1Transport2CwRawBlake3{
    std::byte{0xfa}, std::byte{0xc1}, std::byte{0x6d}, std::byte{0x80}, std::byte{0x24}, std::byte{0xa8}, std::byte{0x63}, std::byte{0xa2},

    std::byte{0x1b}, std::byte{0x69}, std::byte{0x32}, std::byte{0x70}, std::byte{0x59}, std::byte{0x0c}, std::byte{0x03}, std::byte{0xbc},

    std::byte{0x7e}, std::byte{0xcf}, std::byte{0x1e}, std::byte{0xe2}, std::byte{0xd1}, std::byte{0x87}, std::byte{0x21}, std::byte{0xad},

    std::byte{0x80}, std::byte{0x54}, std::byte{0xd3}, std::byte{0xfa}, std::byte{0xec}, std::byte{0x57}, std::byte{0xbd}, std::byte{0x11}
};
inline constexpr std::array<std::byte, 32> kG1Transport2CwPngBlake3{
    std::byte{0x30}, std::byte{0xba}, std::byte{0x61}, std::byte{0x5d}, std::byte{0x40}, std::byte{0x69}, std::byte{0x22}, std::byte{0x73},

    std::byte{0xa4}, std::byte{0x10}, std::byte{0xc1}, std::byte{0x8d}, std::byte{0x7b}, std::byte{0x4a}, std::byte{0x0b}, std::byte{0x18},

    std::byte{0x91}, std::byte{0xe6}, std::byte{0x6f}, std::byte{0xe3}, std::byte{0xfa}, std::byte{0xdf}, std::byte{0x99}, std::byte{0x6c},

    std::byte{0x33}, std::byte{0x51}, std::byte{0x59}, std::byte{0x1e}, std::byte{0xd2}, std::byte{0xe5}, std::byte{0x73}, std::byte{0x57}
};
inline constexpr std::array<std::byte, 32> kG2MaxRawBlake3{
    std::byte{0xcc}, std::byte{0xd2}, std::byte{0x49}, std::byte{0x26}, std::byte{0xd8}, std::byte{0xe5}, std::byte{0x78}, std::byte{0xfc},

    std::byte{0x65}, std::byte{0xdc}, std::byte{0xc4}, std::byte{0xaa}, std::byte{0xea}, std::byte{0xfb}, std::byte{0xee}, std::byte{0xd1},

    std::byte{0xda}, std::byte{0x3c}, std::byte{0xe1}, std::byte{0x9c}, std::byte{0x43}, std::byte{0xa4}, std::byte{0xd5}, std::byte{0x70},

    std::byte{0x94}, std::byte{0x71}, std::byte{0x34}, std::byte{0x4d}, std::byte{0xdc}, std::byte{0x3f}, std::byte{0xfd}, std::byte{0xcc}
};
inline constexpr std::array<std::byte, 32> kG2MaxPngBlake3{
    std::byte{0x91}, std::byte{0x95}, std::byte{0x10}, std::byte{0x01}, std::byte{0x4e}, std::byte{0xca}, std::byte{0xb9}, std::byte{0xfc},

    std::byte{0x5b}, std::byte{0x56}, std::byte{0x24}, std::byte{0x6a}, std::byte{0x8c}, std::byte{0x71}, std::byte{0xd4}, std::byte{0x7e},

    std::byte{0x01}, std::byte{0xf8}, std::byte{0xb9}, std::byte{0x82}, std::byte{0x4c}, std::byte{0x90}, std::byte{0xde}, std::byte{0xbb},

    std::byte{0x4a}, std::byte{0xde}, std::byte{0x11}, std::byte{0x6d}, std::byte{0x86}, std::byte{0xd5}, std::byte{0x44}, std::byte{0xcf}
};

// 275-byte canonical reference region manifest digest.
inline constexpr std::array<std::byte, 32> kManifestBlake3{
    std::byte{0xa7}, std::byte{0xe3}, std::byte{0x1c}, std::byte{0xbd}, std::byte{0x7c}, std::byte{0xfa}, std::byte{0x68}, std::byte{0x65},

    std::byte{0xf8}, std::byte{0xbc}, std::byte{0x02}, std::byte{0x9a}, std::byte{0x78}, std::byte{0xd5}, std::byte{0x8d}, std::byte{0x06},

    std::byte{0x59}, std::byte{0x90}, std::byte{0xe7}, std::byte{0x32}, std::byte{0xf6}, std::byte{0x38}, std::byte{0x66}, std::byte{0x13},

    std::byte{0x32}, std::byte{0x9f}, std::byte{0x2d}, std::byte{0xd0}, std::byte{0x76}, std::byte{0x02}, std::byte{0x00}, std::byte{0x3b}
};

// No-argument fragment wrappers for the recompute dispatch table.
[[nodiscard]] std::vector<std::byte> GenerateControlFragment0()
{
    return GenerateControlFragment(0);
}

[[nodiscard]] std::vector<std::byte> GenerateControlFragment1()
{
    return GenerateControlFragment(1);
}

[[nodiscard]] std::vector<std::byte> GenerateControlFragment2()
{
    return GenerateControlFragment(2);
}

} // namespace

const std::vector<GoldenVectorDescriptor>& GetGoldenVectorRegistry()
{
    static const std::vector<GoldenVectorDescriptor> registry{
        {"bootstrap-record", "protocol", 44, kBootstrapRecordBlake3},
        {"control-sessiondescriptor", "protocol", 67,
            kControlSessionDescriptorBlake3},
        {"control-empty", "protocol", 30, kControlEmptyBlake3},
        {"control-maximum", "protocol", 65536, kControlMaximumBlake3},
        {"control-fragment-0", "protocol", 48, kControlFragment0Blake3},
        {"control-fragment-1", "protocol", 48, kControlFragment1Blake3},
        {"control-fragment-2", "protocol", 43, kControlFragment2Blake3},
        {"session-descriptor", "protocol", 37, kSessionDescriptorBlake3},
        {"segment-descriptor-directrepeat", "protocol", 110,
            kSegmentDescriptorDirectRepeatBlake3},
        {"segment-descriptor-wirehair", "protocol", 142,
            kSegmentDescriptorWirehairBlake3},
        {"final-manifest", "protocol", 65, kFinalManifestBlake3},
        {"wirehair-canonical-descriptor", "protocol", 32,
            kWirehairCanonicalDescriptorBlake3},
        {"transport-block-min", "protocol", 36, kTransportBlockMinBlake3},
        {"transport-block-canonical", "protocol", 1350,
            kTransportBlockCanonicalBlake3},
        {"transport-block-max-payload", "protocol", 65571,
            kTransportBlockMaxPayloadBlake3},
        {"codeword-robust", "ldpc", 2025, kCodewordRobustBlake3},
        {"codeword-balanced", "ldpc", 2025, kCodewordBalancedBlake3},
        {"codeword-fast", "ldpc", 2025, kCodewordFastBlake3},
        {"mapping-sample", "interleave", 8192,
            kInterleaveMappingSampleBlake3},
        {"region-logical", "interleave", 56168,
            kInterleaveRegionLogicalBlake3},
        {"region-physical-phase7", "interleave", 56168,
            kInterleaveRegionPhysicalPhase7Blake3},
        {"reference-raster-manifest", "raster", 275, kManifestBlake3},
        {"g1-transport-info-block", "raster", 1350,
            kG1TransportInfoBlockBlake3},
        {"g1-transport-decoded", "raster", 1350, kG1TransportDecodedBlake3},
        {"g1-transport-2cw-info-block", "raster", 2700,
            kG1Transport2CwInfoBlockBlake3}};
    return registry;
}

const GoldenVectorDescriptor* FindGoldenVector(
    const std::string_view name)
{
    for (const auto& descriptor : GetGoldenVectorRegistry())
    {
        if (descriptor.name == name)
        {
            return &descriptor;
        }
    }
    return nullptr;
}

RecomputeOutcome RecomputeGoldenVector(const std::string_view name)
{
    RecomputeOutcome outcome;
    std::vector<std::byte> (*generator)() = nullptr;
    if (name == "bootstrap-record")
    {
        generator = &GenerateBootstrapRecord;
    }
    else if (name == "control-sessiondescriptor")
    {
        generator = &GenerateControlSessionDescriptor;
    }
    else if (name == "control-empty")
    {
        generator = &GenerateControlEmpty;
    }
    else if (name == "control-maximum")
    {
        generator = &GenerateControlMaximum;
    }
    else if (name == "control-fragment-0")
    {
        generator = &GenerateControlFragment0;
    }
    else if (name == "control-fragment-1")
    {
        generator = &GenerateControlFragment1;
    }
    else if (name == "control-fragment-2")
    {
        generator = &GenerateControlFragment2;
    }
    else if (name == "session-descriptor")
    {
        generator = &GenerateSessionDescriptorPayload;
    }
    else if (name == "segment-descriptor-directrepeat")
    {
        generator = &GenerateDirectRepeatSegmentPayload;
    }
    else if (name == "segment-descriptor-wirehair")
    {
        generator = &GenerateWirehairSegmentPayload;
    }
    else if (name == "final-manifest")
    {
        generator = &GenerateFinalManifestPayload;
    }
    else if (name == "wirehair-canonical-descriptor")
    {
        generator = &GenerateWirehairCanonicalDescriptor;
    }
    else if (name == "transport-block-min")
    {
        generator = &GenerateTransportBlockMinimum;
    }
    else if (name == "transport-block-canonical")
    {
        generator = &GenerateTransportBlockCanonical;
    }
    else if (name == "transport-block-max-payload")
    {
        generator = &GenerateTransportBlockMaxPayload;
    }
    else if (name == "codeword-robust")
    {
        generator = &GenerateLdpcCodewordRobust;
    }
    else if (name == "codeword-balanced")
    {
        generator = &GenerateLdpcCodewordBalanced;
    }
    else if (name == "codeword-fast")
    {
        generator = &GenerateLdpcCodewordFast;
    }
    else if (name == "mapping-sample")
    {
        generator = &GenerateInterleaveMappingSample;
    }
    else if (name == "region-logical")
    {
        generator = &GenerateInterleaveRegionLogical;
    }
    else if (name == "region-physical-phase7")
    {
        generator = &GenerateInterleaveRegionPhysicalPhase7;
    }
    else if (name == "g1-transport-info-block")
    {
        generator = &GenerateG1TransportInfoBlock;
    }
    else if (name == "reference-raster-manifest")
    {
        generator = &GenerateReferenceRasterManifest;
    }
    else if (name == "g1-transport-decoded")
    {
        generator = &GenerateG1TransportDecoded;
    }
    else if (name == "g1-transport-2cw-info-block")
    {
        generator = &GenerateG1Transport2CwInfoBlock;
    }
    else
    {
        return outcome;
    }
    try
    {
        outcome.bytes = generator();
    }
    catch (const std::exception& exception)
    {
        outcome.status = RecomputeStatus::RecomputeFailed;
        outcome.detail = exception.what();
        return outcome;
    }
    catch (...)
    {
        outcome.status = RecomputeStatus::RecomputeFailed;
        outcome.detail = "unknown Golden recompute failure";
        return outcome;
    }
    if (outcome.bytes.empty())
    {
        outcome.status = RecomputeStatus::RecomputeFailed;
        outcome.detail =
            "the implementation recompute produced no bytes (internal "
            "decode/encode chain failure)";
        return outcome;
    }
    outcome.status = RecomputeStatus::Ok;
    return outcome;
}

const std::vector<FrameVectorPin>& GetFrameVectorRegistry()
{
    static const std::vector<FrameVectorPin> registry{
        {"g0-zero", FrameVectorId::G0Zero, kG0ZeroRawBlake3,
            kG0ZeroPngBlake3},
        {"g1-canonical", FrameVectorId::G1Canonical, kG1CanonicalRawBlake3,
            kG1CanonicalPngBlake3},
        {"g1-transport", FrameVectorId::G1Transport, kG1TransportRawBlake3,
            kG1TransportPngBlake3},
        {"g1-transport-2cw", FrameVectorId::G1Transport2Cw,
            kG1Transport2CwRawBlake3, kG1Transport2CwPngBlake3},
        {"g2-max", FrameVectorId::G2Max, kG2MaxRawBlake3,
            kG2MaxPngBlake3}};
    return registry;
}

const std::array<std::byte, 32>& GetManifestDigestPin() noexcept
{
    static const std::array<std::byte, 32> pin = kManifestBlake3;
    return pin;
}

} // namespace pbgolden
