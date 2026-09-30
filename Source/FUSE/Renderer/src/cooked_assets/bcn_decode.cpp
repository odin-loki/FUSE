// E06 (AP-RT-COOKED): see include/fuse/renderer/cooked_assets/bcn_decode.hpp.
#include <fuse/renderer/cooked_assets/bcn_decode.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::cooked_assets {

namespace {

// --- bit reading (little endian, LSB first) ----------------------------------------------------------------
struct Bits {
    const u8* data = nullptr;
    u32 pos = 0;

    u32 take(u32 count) {
        u32 v = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 bit = (data[(pos + i) >> 3u] >> ((pos + i) & 7u)) & 1u;
            v |= bit << i;
        }
        pos += count;
        return v;
    }
};

// --- BC7 / BC6H partition tables (Direct3D 11 functional specification) --------------------------------------
/// Two subsets: bit t set = texel t belongs to subset 1.
constexpr u16 kPartition2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

/// Three subsets: subset of each texel, 64 partitions x 16 texels.
constexpr u8 kPartition3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

/// Anchor texel of subset 1 (two subsets).
constexpr u8 kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
/// Anchor texels of subsets 1 and 2 (three subsets).
constexpr u8 kAnchor3a[64] = {
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,
    6, 10, 5,  8,  8, 6,  8,  5,  15, 15, 8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3,
};
constexpr u8 kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr u32 kWeights2[4] = {0, 21, 43, 64};
constexpr u32 kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr u32 kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const u32* weightTable(u32 bits) {
    return bits == 2u ? kWeights2 : (bits == 3u ? kWeights3 : kWeights4);
}

u32 subsetOf(u32 subsets, u32 partition, u32 texel) {
    if (subsets == 2u) {
        return (kPartition2[partition] >> texel) & 1u;
    }
    if (subsets == 3u) {
        return kPartition3[partition][texel];
    }
    return 0u;
}

bool isAnchor(u32 subsets, u32 partition, u32 texel) {
    if (texel == 0u) {
        return true;
    }
    if (subsets == 2u) {
        return texel == kAnchor2[partition];
    }
    if (subsets == 3u) {
        return texel == kAnchor3a[partition] || texel == kAnchor3b[partition];
    }
    return false;
}

/// Rounded n / d for small non-negative integers.
u32 roundDiv(u32 n, u32 d) { return (2u * n + d) / (2u * d); }

// --- BC1 / BC4 ---------------------------------------------------------------------------------------------
void expand565(u32 c, u32 rgb[3]) {
    const u32 r = (c >> 11u) & 31u;
    const u32 g = (c >> 5u) & 63u;
    const u32 b = c & 31u;
    rgb[0] = (r << 3u) | (r >> 2u);
    rgb[1] = (g << 2u) | (g >> 4u);
    rgb[2] = (b << 3u) | (b >> 2u);
}

// --- BC7 -----------------------------------------------------------------------------------------------------
struct Bc7Mode {
    u32 subsets;
    u32 partitionBits;
    u32 rotationBits;
    u32 indexSelectionBits;
    u32 colorBits;
    u32 alphaBits;
    u32 endpointPBits; ///< one p-bit per endpoint
    u32 sharedPBits;   ///< one p-bit per subset
    u32 indexBits;
    u32 indexBits2;
};

constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

u32 bc7Unquantize(u32 value, u32 bits) {
    value <<= (8u - bits);
    return value | (value >> bits);
}

u32 interpolate(u32 a, u32 b, u32 weight) { return (a * (64u - weight) + b * weight + 32u) >> 6u; }

// --- BC6H ----------------------------------------------------------------------------------------------------
enum Bc6Field : u8 { kD = 0, kRW, kRX, kRY, kRZ, kGW, kGX, kGY, kGZ, kBW, kBX, kBY, kBZ, kFieldCount };

struct Bc6Mode {
    u32 modeValue;      ///< the 2- or 5-bit mode field
    u32 regions;
    bool transformed;
    u32 endpointBits;
    u32 deltaBits[3];   ///< R, G, B
};

// The 14 modes (Direct3D 11 functional specification numbering 1..14, in this order).
constexpr Bc6Mode kBc6Modes[14] = {
    {0x00, 2, true, 10, {5, 5, 5}},   {0x01, 2, true, 7, {6, 6, 6}},    {0x02, 2, true, 11, {5, 4, 4}},
    {0x06, 2, true, 11, {4, 5, 4}},   {0x0a, 2, true, 11, {4, 4, 5}},   {0x0e, 2, true, 9, {5, 5, 5}},
    {0x12, 2, true, 8, {6, 5, 5}},    {0x16, 2, true, 8, {5, 6, 5}},    {0x1a, 2, true, 8, {5, 5, 6}},
    {0x1e, 2, false, 6, {6, 6, 6}},   {0x03, 1, false, 10, {10, 10, 10}}, {0x07, 1, true, 11, {9, 9, 9}},
    {0x0b, 1, true, 12, {8, 8, 8}},   {0x0f, 1, true, 16, {4, 4, 4}},
};

/// Header bit layouts after the mode field, one run list per mode.
struct Bc6Run {
    u8 field;
    u8 hi; ///< first bit read = lo, last = hi, ascending (lo <= hi) or descending when `reversed`
    u8 lo;
    bool reversed;
};

// Each row: runs in stream order (after the 2 / 5 mode bits). {field, hi, lo, reversed}: bits lo..hi of the field,
// read low bit first; reversed: read hi..lo (high bit first).
constexpr Bc6Run kBc6Layout1[] = {
    {kGY, 4, 4, false}, {kBY, 4, 4, false}, {kBZ, 4, 4, false}, {kRW, 9, 0, false}, {kGW, 9, 0, false},
    {kBW, 9, 0, false}, {kRX, 4, 0, false}, {kGZ, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 4, 0, false},
    {kBZ, 0, 0, false}, {kGZ, 3, 0, false}, {kBX, 4, 0, false}, {kBZ, 1, 1, false}, {kBY, 3, 0, false},
    {kRY, 4, 0, false}, {kBZ, 2, 2, false}, {kRZ, 4, 0, false}, {kBZ, 3, 3, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout2[] = {
    {kGY, 5, 5, false}, {kGZ, 4, 4, false}, {kGZ, 5, 5, false}, {kRW, 6, 0, false}, {kBZ, 0, 0, false},
    {kBZ, 1, 1, false}, {kBY, 4, 4, false}, {kGW, 6, 0, false}, {kBY, 5, 5, false}, {kBZ, 2, 2, false},
    {kGY, 4, 4, false}, {kBW, 6, 0, false}, {kBZ, 3, 3, false}, {kBZ, 5, 5, false}, {kBZ, 4, 4, false},
    {kRX, 5, 0, false}, {kGY, 3, 0, false}, {kGX, 5, 0, false}, {kGZ, 3, 0, false}, {kBX, 5, 0, false},
    {kBY, 3, 0, false}, {kRY, 5, 0, false}, {kRZ, 5, 0, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout3[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 4, 0, false}, {kRW, 10, 10, false},
    {kGY, 3, 0, false}, {kGX, 3, 0, false}, {kGW, 10, 10, false}, {kBZ, 0, 0, false}, {kGZ, 3, 0, false},
    {kBX, 3, 0, false}, {kBW, 10, 10, false}, {kBZ, 1, 1, false}, {kBY, 3, 0, false}, {kRY, 4, 0, false},
    {kBZ, 2, 2, false}, {kRZ, 4, 0, false}, {kBZ, 3, 3, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout4[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 3, 0, false}, {kRW, 10, 10, false},
    {kGZ, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 4, 0, false}, {kGW, 10, 10, false}, {kGZ, 3, 0, false},
    {kBX, 3, 0, false}, {kBW, 10, 10, false}, {kBZ, 1, 1, false}, {kBY, 3, 0, false}, {kRY, 3, 0, false},
    {kBZ, 0, 0, false}, {kBZ, 2, 2, false}, {kRZ, 3, 0, false}, {kGY, 4, 4, false}, {kBZ, 3, 3, false},
    {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout5[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 3, 0, false}, {kRW, 10, 10, false},
    {kBY, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 3, 0, false}, {kGW, 10, 10, false}, {kBZ, 0, 0, false},
    {kGZ, 3, 0, false}, {kBX, 4, 0, false}, {kBW, 10, 10, false}, {kBY, 3, 0, false}, {kRY, 3, 0, false},
    {kBZ, 1, 1, false}, {kBZ, 2, 2, false}, {kRZ, 3, 0, false}, {kBZ, 4, 4, false}, {kBZ, 3, 3, false},
    {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout6[] = {
    {kRW, 8, 0, false}, {kBY, 4, 4, false}, {kGW, 8, 0, false}, {kGY, 4, 4, false}, {kBW, 8, 0, false},
    {kBZ, 4, 4, false}, {kRX, 4, 0, false}, {kGZ, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 4, 0, false},
    {kBZ, 0, 0, false}, {kGZ, 3, 0, false}, {kBX, 4, 0, false}, {kBZ, 1, 1, false}, {kBY, 3, 0, false},
    {kRY, 4, 0, false}, {kBZ, 2, 2, false}, {kRZ, 4, 0, false}, {kBZ, 3, 3, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout7[] = {
    {kRW, 7, 0, false}, {kGZ, 4, 4, false}, {kBY, 4, 4, false}, {kGW, 7, 0, false}, {kBZ, 2, 2, false},
    {kGY, 4, 4, false}, {kBW, 7, 0, false}, {kBZ, 3, 3, false}, {kBZ, 4, 4, false}, {kRX, 5, 0, false},
    {kGY, 3, 0, false}, {kGX, 4, 0, false}, {kBZ, 0, 0, false}, {kGZ, 3, 0, false}, {kBX, 4, 0, false},
    {kBZ, 1, 1, false}, {kBY, 3, 0, false}, {kRY, 5, 0, false}, {kRZ, 5, 0, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout8[] = {
    {kRW, 7, 0, false}, {kBZ, 0, 0, false}, {kBY, 4, 4, false}, {kGW, 7, 0, false}, {kGY, 5, 5, false},
    {kGY, 4, 4, false}, {kBW, 7, 0, false}, {kGZ, 5, 5, false}, {kBZ, 4, 4, false}, {kRX, 4, 0, false},
    {kGZ, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 5, 0, false}, {kGZ, 3, 0, false}, {kBX, 4, 0, false},
    {kBZ, 1, 1, false}, {kBY, 3, 0, false}, {kRY, 4, 0, false}, {kBZ, 2, 2, false}, {kRZ, 4, 0, false},
    {kBZ, 3, 3, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout9[] = {
    {kRW, 7, 0, false}, {kBZ, 1, 1, false}, {kBY, 4, 4, false}, {kGW, 7, 0, false}, {kBY, 5, 5, false},
    {kGY, 4, 4, false}, {kBW, 7, 0, false}, {kBZ, 5, 5, false}, {kBZ, 4, 4, false}, {kRX, 4, 0, false},
    {kGZ, 4, 4, false}, {kGY, 3, 0, false}, {kGX, 4, 0, false}, {kBZ, 0, 0, false}, {kGZ, 3, 0, false},
    {kBX, 5, 0, false}, {kBY, 3, 0, false}, {kRY, 4, 0, false}, {kBZ, 2, 2, false}, {kRZ, 4, 0, false},
    {kBZ, 3, 3, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout10[] = {
    {kRW, 5, 0, false}, {kGZ, 4, 4, false}, {kBZ, 0, 0, false}, {kBZ, 1, 1, false}, {kBY, 4, 4, false},
    {kGW, 5, 0, false}, {kGY, 5, 5, false}, {kBY, 5, 5, false}, {kBZ, 2, 2, false}, {kGY, 4, 4, false},
    {kBW, 5, 0, false}, {kGZ, 5, 5, false}, {kBZ, 3, 3, false}, {kBZ, 5, 5, false}, {kBZ, 4, 4, false},
    {kRX, 5, 0, false}, {kGY, 3, 0, false}, {kGX, 5, 0, false}, {kGZ, 3, 0, false}, {kBX, 5, 0, false},
    {kBY, 3, 0, false}, {kRY, 5, 0, false}, {kRZ, 5, 0, false}, {kD, 4, 0, false},
};
constexpr Bc6Run kBc6Layout11[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false},
    {kRX, 9, 0, false}, {kGX, 9, 0, false}, {kBX, 9, 0, false},
};
constexpr Bc6Run kBc6Layout12[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 8, 0, false}, {kRW, 10, 10, false},
    {kGX, 8, 0, false}, {kGW, 10, 10, false}, {kBX, 8, 0, false}, {kBW, 10, 10, false},
};
constexpr Bc6Run kBc6Layout13[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 7, 0, false}, {kRW, 11, 10, true},
    {kGX, 7, 0, false}, {kGW, 11, 10, true}, {kBX, 7, 0, false}, {kBW, 11, 10, true},
};
constexpr Bc6Run kBc6Layout14[] = {
    {kRW, 9, 0, false}, {kGW, 9, 0, false}, {kBW, 9, 0, false}, {kRX, 3, 0, false}, {kRW, 15, 10, true},
    {kGX, 3, 0, false}, {kGW, 15, 10, true}, {kBX, 3, 0, false}, {kBW, 15, 10, true},
};

struct Bc6LayoutRef {
    const Bc6Run* runs;
    u32 count;
};
template <usize N>
constexpr Bc6LayoutRef layoutOf(const Bc6Run (&runs)[N]) {
    return Bc6LayoutRef{runs, static_cast<u32>(N)};
}
constexpr Bc6LayoutRef kBc6Layouts[14] = {
    layoutOf(kBc6Layout1),  layoutOf(kBc6Layout2),  layoutOf(kBc6Layout3),  layoutOf(kBc6Layout4),
    layoutOf(kBc6Layout5),  layoutOf(kBc6Layout6),  layoutOf(kBc6Layout7),  layoutOf(kBc6Layout8),
    layoutOf(kBc6Layout9),  layoutOf(kBc6Layout10), layoutOf(kBc6Layout11), layoutOf(kBc6Layout12),
    layoutOf(kBc6Layout13), layoutOf(kBc6Layout14),
};

s32 signExtend(u32 value, u32 bits) {
    if (bits == 0u || bits >= 32u) {
        return static_cast<s32>(value);
    }
    const u32 mask = (1u << bits) - 1u;
    value &= mask;
    const u32 sign = 1u << (bits - 1u);
    return static_cast<s32>((value ^ sign)) - static_cast<s32>(sign);
}

s32 bc6Unquantize(s32 value, u32 bits, bool isSigned) {
    if (!isSigned) {
        if (bits >= 15u) {
            return value;
        }
        if (value == 0) {
            return 0;
        }
        if (value == static_cast<s32>((1u << bits) - 1u)) {
            return 0xFFFF;
        }
        return static_cast<s32>(((static_cast<u32>(value) << 15u) + 0x4000u) >> (bits - 1u));
    }
    if (bits >= 16u) {
        return value;
    }
    bool negative = false;
    if (value < 0) {
        negative = true;
        value = -value;
    }
    s32 q = 0;
    if (value == 0) {
        q = 0;
    } else if (value >= static_cast<s32>((1u << (bits - 1u)) - 1u)) {
        q = 0x7FFF;
    } else {
        q = static_cast<s32>(((static_cast<u32>(value) << 15u) + 0x4000u) >> (bits - 1u));
    }
    return negative ? -q : q;
}

u16 bc6Finish(s32 value, bool isSigned) {
    if (!isSigned) {
        return static_cast<u16>((static_cast<u32>(value) * 31u) >> 6u);
    }
    u32 sign = 0u;
    if (value < 0) {
        sign = 0x8000u;
        value = -value;
    }
    return static_cast<u16>(sign | ((static_cast<u32>(value) * 31u) >> 5u));
}

} // namespace

// ================================================================================================================
void bc1_decode_block(const u8 block[8], u8 rgba[64]) {
    const u32 c0 = static_cast<u32>(block[0]) | (static_cast<u32>(block[1]) << 8u);
    const u32 c1 = static_cast<u32>(block[2]) | (static_cast<u32>(block[3]) << 8u);
    u32 palette[4][4];
    expand565(c0, palette[0]);
    expand565(c1, palette[1]);
    palette[0][3] = 255u;
    palette[1][3] = 255u;
    for (u32 c = 0; c < 3u; ++c) {
        if (c0 > c1) {
            palette[2][c] = roundDiv(2u * palette[0][c] + palette[1][c], 3u);
            palette[3][c] = roundDiv(palette[0][c] + 2u * palette[1][c], 3u);
        } else {
            palette[2][c] = roundDiv(palette[0][c] + palette[1][c], 2u);
            palette[3][c] = 0u; // BC1_RGB: the "transparent" entry is opaque black
        }
    }
    palette[2][3] = 255u;
    palette[3][3] = 255u;
    const u32 indices = static_cast<u32>(block[4]) | (static_cast<u32>(block[5]) << 8u) |
                        (static_cast<u32>(block[6]) << 16u) | (static_cast<u32>(block[7]) << 24u);
    for (u32 t = 0; t < 16u; ++t) {
        const u32 i = (indices >> (2u * t)) & 3u;
        for (u32 c = 0; c < 4u; ++c) {
            rgba[t * 4u + c] = static_cast<u8>(palette[i][c]);
        }
    }
}

void bc4_decode_block(const u8 block[8], u8 values[16]) {
    const u32 r0 = block[0];
    const u32 r1 = block[1];
    u32 palette[8];
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (u32 i = 2; i < 8u; ++i) {
            palette[i] = roundDiv((8u - i) * r0 + (i - 1u) * r1, 7u);
        }
    } else {
        for (u32 i = 2; i < 6u; ++i) {
            palette[i] = roundDiv((6u - i) * r0 + (i - 1u) * r1, 5u);
        }
        palette[6] = 0u;
        palette[7] = 255u;
    }
    u64 bits = 0;
    for (u32 i = 0; i < 6u; ++i) {
        bits |= static_cast<u64>(block[2u + i]) << (8u * i);
    }
    for (u32 t = 0; t < 16u; ++t) {
        values[t] = static_cast<u8>(palette[(bits >> (3u * t)) & 7u]);
    }
}

void bc5_decode_block(const u8 block[16], u8 rgba[64]) {
    u8 r[16];
    u8 g[16];
    bc4_decode_block(block, r);
    bc4_decode_block(block + 8, g);
    for (u32 t = 0; t < 16u; ++t) {
        rgba[t * 4u + 0] = r[t];
        rgba[t * 4u + 1] = g[t];
        rgba[t * 4u + 2] = 0u;
        rgba[t * 4u + 3] = 255u;
    }
}

void bc7_decode_block(const u8 block[16], u8 rgba[64]) {
    Bits bits{block, 0};
    u32 mode = 0;
    while (mode < 8u && bits.take(1) == 0u) {
        ++mode;
    }
    if (mode >= 8u) {
        std::memset(rgba, 0, 64);
        return;
    }
    const Bc7Mode& m = kBc7Modes[mode];
    const u32 partition = bits.take(m.partitionBits);
    const u32 rotation = bits.take(m.rotationBits);
    const u32 indexSelection = bits.take(m.indexSelectionBits);
    u32 ep[3][2][4] = {}; // subset, endpoint, channel (quantised, then expanded)
    for (u32 c = 0; c < 3u; ++c) {
        for (u32 s = 0; s < m.subsets; ++s) {
            ep[s][0][c] = bits.take(m.colorBits);
            ep[s][1][c] = bits.take(m.colorBits);
        }
    }
    if (m.alphaBits != 0u) {
        for (u32 s = 0; s < m.subsets; ++s) {
            ep[s][0][3] = bits.take(m.alphaBits);
            ep[s][1][3] = bits.take(m.alphaBits);
        }
    }
    u32 pbit[3][2] = {};
    if (m.endpointPBits != 0u) {
        for (u32 s = 0; s < m.subsets; ++s) {
            pbit[s][0] = bits.take(1);
            pbit[s][1] = bits.take(1);
        }
    } else if (m.sharedPBits != 0u) {
        for (u32 s = 0; s < m.subsets; ++s) {
            pbit[s][0] = pbit[s][1] = bits.take(1);
        }
    }
    const bool hasP = m.endpointPBits != 0u || m.sharedPBits != 0u;
    for (u32 s = 0; s < m.subsets; ++s) {
        for (u32 e = 0; e < 2u; ++e) {
            for (u32 c = 0; c < 3u; ++c) {
                const u32 v = hasP ? (ep[s][e][c] << 1u) | pbit[s][e] : ep[s][e][c];
                ep[s][e][c] = bc7Unquantize(v, m.colorBits + (hasP ? 1u : 0u));
            }
            if (m.alphaBits != 0u) {
                const u32 v = hasP ? (ep[s][e][3] << 1u) | pbit[s][e] : ep[s][e][3];
                ep[s][e][3] = bc7Unquantize(v, m.alphaBits + (hasP ? 1u : 0u));
            } else {
                ep[s][e][3] = 255u;
            }
        }
    }
    u32 index1[16] = {};
    u32 index2[16] = {};
    for (u32 t = 0; t < 16u; ++t) {
        const bool anchor = isAnchor(m.subsets, partition, t);
        index1[t] = bits.take(anchor ? m.indexBits - 1u : m.indexBits);
    }
    if (m.indexBits2 != 0u) {
        for (u32 t = 0; t < 16u; ++t) {
            index2[t] = bits.take(t == 0u ? m.indexBits2 - 1u : m.indexBits2);
        }
    }
    for (u32 t = 0; t < 16u; ++t) {
        const u32 s = subsetOf(m.subsets, partition, t);
        u32 out[4];
        if (m.indexBits2 == 0u) {
            const u32* w = weightTable(m.indexBits);
            for (u32 c = 0; c < 4u; ++c) {
                out[c] = interpolate(ep[s][0][c], ep[s][1][c], w[index1[t]]);
            }
        } else {
            // Modes 4 / 5: colour and alpha use separate index sets (mode 4 may swap them).
            const bool swap = indexSelection != 0u;
            const u32 colorIndex = swap ? index2[t] : index1[t];
            const u32 alphaIndex = swap ? index1[t] : index2[t];
            const u32* wc = weightTable(swap ? m.indexBits2 : m.indexBits);
            const u32* wa = weightTable(swap ? m.indexBits : m.indexBits2);
            for (u32 c = 0; c < 3u; ++c) {
                out[c] = interpolate(ep[s][0][c], ep[s][1][c], wc[colorIndex]);
            }
            out[3] = interpolate(ep[s][0][3], ep[s][1][3], wa[alphaIndex]);
        }
        if (rotation != 0u) {
            const u32 tmp = out[3];
            out[3] = out[rotation - 1u];
            out[rotation - 1u] = tmp;
        }
        for (u32 c = 0; c < 4u; ++c) {
            rgba[t * 4u + c] = static_cast<u8>(out[c]);
        }
    }
}

void bc6h_decode_block(const u8 block[16], u16 rgba[64], bool isSigned) {
    constexpr u16 kHalfOne = 0x3C00u;
    Bits bits{block, 0};
    u32 modeValue = bits.take(2);
    if (modeValue >= 2u) {
        modeValue |= bits.take(3) << 2u;
    }
    u32 modeIndex = 14u;
    for (u32 i = 0; i < 14u; ++i) {
        if (kBc6Modes[i].modeValue == modeValue) {
            modeIndex = i;
            break;
        }
    }
    if (modeIndex >= 14u) {
        for (u32 t = 0; t < 16u; ++t) {
            rgba[t * 4u + 0] = 0u;
            rgba[t * 4u + 1] = 0u;
            rgba[t * 4u + 2] = 0u;
            rgba[t * 4u + 3] = kHalfOne;
        }
        return;
    }
    const Bc6Mode& m = kBc6Modes[modeIndex];
    u32 field[kFieldCount] = {};
    const Bc6LayoutRef layout = kBc6Layouts[modeIndex];
    for (u32 r = 0; r < layout.count; ++r) {
        const Bc6Run& run = layout.runs[r];
        if (run.reversed) {
            for (u32 b = run.hi + 1u; b-- > run.lo;) {
                field[run.field] |= bits.take(1) << b;
            }
        } else {
            for (u32 b = run.lo; b <= run.hi; ++b) {
                field[run.field] |= bits.take(1) << b;
            }
        }
    }
    const u32 partition = m.regions == 2u ? field[kD] : 0u;
    // Endpoints: [region * 2 + endpoint][channel], raw.
    const u32 wFields[3] = {field[kRW], field[kGW], field[kBW]};
    const u32 xFields[3] = {field[kRX], field[kGX], field[kBX]};
    const u32 yFields[3] = {field[kRY], field[kGY], field[kBY]};
    const u32 zFields[3] = {field[kRZ], field[kGZ], field[kBZ]};
    s32 ends[4][3] = {};
    const u32 epb = m.endpointBits;
    const u32 epMask = epb >= 32u ? 0xFFFFFFFFu : (1u << epb) - 1u;
    for (u32 c = 0; c < 3u; ++c) {
        const s32 w = isSigned ? signExtend(wFields[c], epb) : static_cast<s32>(wFields[c]);
        ends[0][c] = w;
        const u32 others[3] = {xFields[c], yFields[c], zFields[c]};
        const u32 count = m.regions == 2u ? 3u : 1u;
        for (u32 k = 0; k < count; ++k) {
            s32 v = 0;
            if (m.transformed) {
                const s32 delta = signExtend(others[k], m.deltaBits[c]);
                const u32 sum = (wFields[c] + static_cast<u32>(delta)) & epMask;
                v = isSigned ? signExtend(sum, epb) : static_cast<s32>(sum);
            } else {
                v = isSigned ? signExtend(others[k], epb) : static_cast<s32>(others[k]);
            }
            ends[k + 1u][c] = v;
        }
    }
    for (u32 e = 0; e < 4u; ++e) {
        for (u32 c = 0; c < 3u; ++c) {
            ends[e][c] = bc6Unquantize(ends[e][c], epb, isSigned);
        }
    }
    const u32 indexBits = m.regions == 2u ? 3u : 4u;
    const u32* weights = weightTable(indexBits);
    for (u32 t = 0; t < 16u; ++t) {
        const bool anchor = m.regions == 2u ? (t == 0u || t == kAnchor2[partition]) : t == 0u;
        const u32 index = bits.take(anchor ? indexBits - 1u : indexBits);
        const u32 region = m.regions == 2u ? ((kPartition2[partition] >> t) & 1u) : 0u;
        const u32 w = weights[index];
        for (u32 c = 0; c < 3u; ++c) {
            const s32 a = ends[region * 2u][c];
            const s32 b = ends[region * 2u + 1u][c];
            const s32 v = (a * static_cast<s32>(64u - w) + b * static_cast<s32>(w) + 32) >> 6;
            rgba[t * 4u + c] = bc6Finish(v, isSigned);
        }
        rgba[t * 4u + 3] = kHalfOne;
    }
}

u32 bc_decoded_texel_bytes(asset::BcFormat format) { return format == asset::BcFormat::BC6H ? 8u : 4u; }

bool decode_bc_level(asset::BcFormat format, const u8* blocks, usize blockBytes, u32 width, u32 height, u32 layers,
                     u8* out, bool signedBc6h) {
    const u32 bw = (width + 3u) / 4u;
    const u32 bh = (height + 3u) / 4u;
    const u32 bb = asset::bc_block_bytes(format);
    const usize needed = static_cast<usize>(bw) * bh * bb * layers;
    if (blocks == nullptr || out == nullptr || blockBytes < needed || width == 0u || height == 0u) {
        return false;
    }
    const u32 tb = bc_decoded_texel_bytes(format);
    const usize layerTexelBytes = static_cast<usize>(width) * height * tb;
    for (u32 layer = 0; layer < layers; ++layer) {
        const u8* src = blocks + static_cast<usize>(layer) * bw * bh * bb;
        u8* dst = out + layer * layerTexelBytes;
        for (u32 by = 0; by < bh; ++by) {
            for (u32 bx = 0; bx < bw; ++bx) {
                const u8* block = src + (static_cast<usize>(by) * bw + bx) * bb;
                u8 tile8[64];
                u16 tile16[64];
                switch (format) {
                case asset::BcFormat::BC1:
                    bc1_decode_block(block, tile8);
                    break;
                case asset::BcFormat::BC4: {
                    u8 v[16];
                    bc4_decode_block(block, v);
                    for (u32 t = 0; t < 16u; ++t) {
                        tile8[t * 4u + 0] = v[t];
                        tile8[t * 4u + 1] = 0u;
                        tile8[t * 4u + 2] = 0u;
                        tile8[t * 4u + 3] = 255u;
                    }
                    break;
                }
                case asset::BcFormat::BC5:
                    bc5_decode_block(block, tile8);
                    break;
                case asset::BcFormat::BC6H:
                    bc6h_decode_block(block, tile16, signedBc6h);
                    break;
                case asset::BcFormat::BC7:
                    bc7_decode_block(block, tile8);
                    break;
                }
                for (u32 ty = 0; ty < 4u; ++ty) {
                    const u32 y = by * 4u + ty;
                    if (y >= height) {
                        break;
                    }
                    for (u32 tx = 0; tx < 4u; ++tx) {
                        const u32 x = bx * 4u + tx;
                        if (x >= width) {
                            break;
                        }
                        u8* texel = dst + (static_cast<usize>(y) * width + x) * tb;
                        if (tb == 8u) {
                            std::memcpy(texel, &tile16[(ty * 4u + tx) * 4u], 8u);
                        } else {
                            std::memcpy(texel, &tile8[(ty * 4u + tx) * 4u], 4u);
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool decode_cooked_texture(const asset::CookedTexture& texture, DecodedTexture& out) {
    out = DecodedTexture{};
    if (texture.levels.empty() || texture.layers == 0u) {
        return false;
    }
    out.width = texture.width;
    out.height = texture.height;
    out.layers = texture.layers;
    out.levels = static_cast<u32>(texture.levels.size());
    out.texelBytes = bc_decoded_texel_bytes(texture.format);
    usize total = 0;
    for (const asset::CookedTexture::Level& level : texture.levels) {
        out.levelOffsets.push_back(total);
        total += static_cast<usize>(level.width) * level.height * texture.layers * out.texelBytes;
    }
    out.bytes.resize(total);
    for (usize l = 0; l < texture.levels.size(); ++l) {
        const asset::CookedTexture::Level& level = texture.levels[l];
        if (!decode_bc_level(texture.format, level.blocks.data(), level.blocks.size(), level.width, level.height,
                             texture.layers, out.bytes.data() + out.levelOffsets[l])) {
            out = DecodedTexture{};
            return false;
        }
    }
    return true;
}

f32 half_bits_to_float(u16 bits) {
    const u32 sign = (bits >> 15u) & 1u;
    const u32 exponent = (bits >> 10u) & 31u;
    const u32 mantissa = bits & 1023u;
    f32 magnitude = 0.f;
    if (exponent == 0u) {
        magnitude = std::ldexp(static_cast<f32>(mantissa), -24);
    } else if (exponent == 31u) {
        magnitude = mantissa == 0u ? INFINITY : NAN;
    } else {
        magnitude = std::ldexp(static_cast<f32>(mantissa | 1024u), static_cast<int>(exponent) - 25);
    }
    return sign != 0u ? -magnitude : magnitude;
}

bool decode_cooked_level_rgba8(const asset::CookedTexture& texture, u32 level, u32 layer, std::vector<u32>& out) {
    out.clear();
    if (level >= texture.levels.size() || layer >= texture.layers) {
        return false;
    }
    const asset::CookedTexture::Level& l = texture.levels[level];
    const u32 tb = bc_decoded_texel_bytes(texture.format);
    const usize layerTexels = static_cast<usize>(l.width) * l.height;
    std::vector<u8> decoded(layerTexels * texture.layers * tb);
    if (!decode_bc_level(texture.format, l.blocks.data(), l.blocks.size(), l.width, l.height, texture.layers,
                         decoded.data())) {
        return false;
    }
    out.resize(layerTexels);
    const u8* src = decoded.data() + layerTexels * layer * tb;
    for (usize i = 0; i < layerTexels; ++i) {
        u32 word = 0u;
        if (tb == 8u) {
            for (u32 c = 0; c < 4u; ++c) {
                u16 h = 0;
                std::memcpy(&h, src + i * 8u + c * 2u, 2u);
                const f32 v = half_bits_to_float(h);
                const f32 clamped = v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; // NaN -> 0
                word |= static_cast<u32>(std::lround(clamped * 255.f)) << (8u * c);
            }
        } else {
            std::memcpy(&word, src + i * 4u, 4u);
        }
        out[i] = word;
    }
    return true;
}

} // namespace fuse::renderer::cooked_assets
