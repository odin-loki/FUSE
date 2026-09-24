// FUSE Relight RL-3.3: CPU texture decode (see texture_decode.hpp). FUSE's own implementation of the
// Direct3D 11 block-compression specification; the BC6H/BC7 tables below are specification data.
#include <fuse/relight/mods/assets/texture_decode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::relight::mods::assets {

namespace {

std::uint32_t load32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t load64(const std::uint8_t* p) { return std::uint64_t(load32(p)) | (std::uint64_t(load32(p + 4)) << 32); }

std::uint8_t expand(std::uint32_t v, int bits) {
    const std::uint32_t maxv = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((v * 255u + maxv / 2u) / maxv);
}

/// LSB-first reader over a 128-bit block.
class BitReader {
public:
    explicit BitReader(const std::uint8_t* block) : lo_(load64(block)), hi_(load64(block + 8)) {}
    std::uint32_t read(int n) {
        if (n == 0) {
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(lo_ & ((std::uint64_t(1) << n) - 1));
        lo_ = (lo_ >> n) | (hi_ << (64 - n));
        hi_ >>= n;
        return v;
    }

private:
    std::uint64_t lo_, hi_;
};

// ---- BC6H / BC7 specification tables ---------------------------------------------------------------------

// Partition shapes: 2 bits per texel (texel i at bits 2i..2i+1) = subset index.
constexpr std::uint32_t kPartition2[64] = {
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000, 0x50400000, 0x55555450,
    0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000, 0x55150100, 0x00004054, 0x15010000, 0x00405054,
    0x00004050, 0x15050100, 0x05010000, 0x40505054, 0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500,
    0x15014054, 0x05414150, 0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400, 0x00041504, 0x00105410,
    0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150, 0x41050514, 0x41505014, 0x40011554, 0x54150140,
    0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
constexpr std::uint32_t kPartition3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050, 0xaa550000, 0xaa555500,
    0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0,
    0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414,
    0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444, 0x54a854a8,
    0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000,
    0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
// Anchor (fix-up) texel of subset 1 for 2-subset shapes, and of subsets 1 and 2 for 3-subset shapes.
constexpr std::uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                                       8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8, 2,  2,
                                       2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8,  3,  15, 15, 8,  8, 6,  6,  6,  5,  3, 3,  3,  3,  8,  15, 3, 3,
                                        6, 10, 5,  8,  8,  6,  8,  5,  15, 15, 8, 15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,
                                        15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5,  10, 5,  10, 8, 13, 15, 12, 3,  3};
constexpr std::uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                                        15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                                        3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

constexpr int kWeights2[4] = {0, 21, 43, 64};
constexpr int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const int* weightTable(int bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

int interpolate(int a, int b, const int* w, int index) { return (a * (64 - w[index]) + b * w[index] + 32) >> 6; }

std::uint32_t subsetOf(int numSubsets, int partition, int texel) {
    if (numSubsets == 1) {
        return 0;
    }
    const std::uint32_t shape = numSubsets == 2 ? kPartition2[partition] : kPartition3[partition];
    return (shape >> (2 * texel)) & 3u;
}

bool isAnchor(int numSubsets, int partition, int texel) {
    if (texel == 0) {
        return true;
    }
    if (numSubsets == 2) {
        return texel == kAnchor2[partition];
    }
    if (numSubsets == 3) {
        return texel == kAnchor3a[partition] || texel == kAnchor3b[partition];
    }
    return false;
}

// BC6H mode layouts (D3D11 functional spec 19.5.9, "BC6H" mode table): each entry reads `count` bits into
// field `field` starting at bit `shift` (reversed = the bits are stored MSB-first).
enum Field : std::uint8_t { F_R0, F_G0, F_B0, F_R1, F_G1, F_B1, F_R2, F_G2, F_B2, F_R3, F_G3, F_B3, F_P };
struct Bits {
    std::uint8_t field, shift, count, reversed;
};
struct Bc6Mode {
    std::uint8_t code;       ///< mode bits (2 or 5)
    std::uint8_t subsets;    ///< 1 or 2
    std::uint8_t transformed; ///< endpoints 1..3 are deltas from endpoint 0
    std::uint8_t epBits;     ///< precision of endpoint 0 (the unquantize precision)
    std::uint8_t deltaBits[3]; ///< r, g, b precision of the other endpoints
    Bits layout[24];
};

constexpr Bc6Mode kBc6Modes[14] = {
    {0x00, 2, 1, 10, {5, 5, 5},
     {{F_G2,4,1,0},{F_B2,4,1,0},{F_B3,4,1,0},{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,5,0},{F_G3,4,1,0},{F_G2,0,4,0},
      {F_G1,0,5,0},{F_B3,0,1,0},{F_G3,0,4,0},{F_B1,0,5,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,5,0},{F_B3,2,1,0},{F_R3,0,5,0},
      {F_B3,3,1,0},{F_P,0,5,0}}},
    {0x01, 2, 1, 7, {6, 6, 6},
     {{F_G2,5,1,0},{F_G3,4,1,0},{F_G3,5,1,0},{F_R0,0,7,0},{F_B3,0,1,0},{F_B3,1,1,0},{F_B2,4,1,0},{F_G0,0,7,0},{F_B2,5,1,0},
      {F_B3,2,1,0},{F_G2,4,1,0},{F_B0,0,7,0},{F_B3,3,1,0},{F_B3,5,1,0},{F_B3,4,1,0},{F_R1,0,6,0},{F_G2,0,4,0},{F_G1,0,6,0},
      {F_G3,0,4,0},{F_B1,0,6,0},{F_B2,0,4,0},{F_R2,0,6,0},{F_R3,0,6,0},{F_P,0,5,0}}},
    {0x02, 2, 1, 11, {5, 4, 4},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,5,0},{F_R0,10,1,0},{F_G2,0,4,0},{F_G1,0,4,0},{F_G0,10,1,0},{F_B3,0,1,0},
      {F_G3,0,4,0},{F_B1,0,4,0},{F_B0,10,1,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,5,0},{F_B3,2,1,0},{F_R3,0,5,0},{F_B3,3,1,0},
      {F_P,0,5,0}}},
    {0x06, 2, 1, 11, {4, 5, 4},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,4,0},{F_R0,10,1,0},{F_G3,4,1,0},{F_G2,0,4,0},{F_G1,0,5,0},{F_G0,10,1,0},
      {F_G3,0,4,0},{F_B1,0,4,0},{F_B0,10,1,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,4,0},{F_B3,0,1,0},{F_B3,2,1,0},{F_R3,0,4,0},
      {F_G2,4,1,0},{F_B3,3,1,0},{F_P,0,5,0}}},
    {0x0a, 2, 1, 11, {4, 4, 5},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,4,0},{F_R0,10,1,0},{F_B2,4,1,0},{F_G2,0,4,0},{F_G1,0,4,0},{F_G0,10,1,0},
      {F_B3,0,1,0},{F_G3,0,4,0},{F_B1,0,5,0},{F_B0,10,1,0},{F_B2,0,4,0},{F_R2,0,4,0},{F_B3,1,1,0},{F_B3,2,1,0},{F_R3,0,4,0},
      {F_B3,4,1,0},{F_B3,3,1,0},{F_P,0,5,0}}},
    {0x0e, 2, 1, 9, {5, 5, 5},
     {{F_R0,0,9,0},{F_B2,4,1,0},{F_G0,0,9,0},{F_G2,4,1,0},{F_B0,0,9,0},{F_B3,4,1,0},{F_R1,0,5,0},{F_G3,4,1,0},{F_G2,0,4,0},
      {F_G1,0,5,0},{F_B3,0,1,0},{F_G3,0,4,0},{F_B1,0,5,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,5,0},{F_B3,2,1,0},{F_R3,0,5,0},
      {F_B3,3,1,0},{F_P,0,5,0}}},
    {0x12, 2, 1, 8, {6, 5, 5},
     {{F_R0,0,8,0},{F_G3,4,1,0},{F_B2,4,1,0},{F_G0,0,8,0},{F_B3,2,1,0},{F_G2,4,1,0},{F_B0,0,8,0},{F_B3,3,1,0},{F_B3,4,1,0},
      {F_R1,0,6,0},{F_G2,0,4,0},{F_G1,0,5,0},{F_B3,0,1,0},{F_G3,0,4,0},{F_B1,0,5,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,6,0},
      {F_R3,0,6,0},{F_P,0,5,0}}},
    {0x16, 2, 1, 8, {5, 6, 5},
     {{F_R0,0,8,0},{F_B3,0,1,0},{F_B2,4,1,0},{F_G0,0,8,0},{F_G2,5,1,0},{F_G2,4,1,0},{F_B0,0,8,0},{F_G3,5,1,0},{F_B3,4,1,0},
      {F_R1,0,5,0},{F_G3,4,1,0},{F_G2,0,4,0},{F_G1,0,6,0},{F_G3,0,4,0},{F_B1,0,5,0},{F_B3,1,1,0},{F_B2,0,4,0},{F_R2,0,5,0},
      {F_B3,2,1,0},{F_R3,0,5,0},{F_B3,3,1,0},{F_P,0,5,0}}},
    {0x1a, 2, 1, 8, {5, 5, 6},
     {{F_R0,0,8,0},{F_B3,1,1,0},{F_B2,4,1,0},{F_G0,0,8,0},{F_B2,5,1,0},{F_G2,4,1,0},{F_B0,0,8,0},{F_B3,5,1,0},{F_B3,4,1,0},
      {F_R1,0,5,0},{F_G3,4,1,0},{F_G2,0,4,0},{F_G1,0,5,0},{F_B3,0,1,0},{F_G3,0,4,0},{F_B1,0,6,0},{F_B2,0,4,0},{F_R2,0,5,0},
      {F_B3,2,1,0},{F_R3,0,5,0},{F_B3,3,1,0},{F_P,0,5,0}}},
    {0x1e, 2, 0, 6, {6, 6, 6},
     {{F_R0,0,6,0},{F_G3,4,1,0},{F_B3,0,1,0},{F_B3,1,1,0},{F_B2,4,1,0},{F_G0,0,6,0},{F_G2,5,1,0},{F_B2,5,1,0},{F_B3,2,1,0},
      {F_G2,4,1,0},{F_B0,0,6,0},{F_G3,5,1,0},{F_B3,3,1,0},{F_B3,5,1,0},{F_B3,4,1,0},{F_R1,0,6,0},{F_G2,0,4,0},{F_G1,0,6,0},
      {F_G3,0,4,0},{F_B1,0,6,0},{F_B2,0,4,0},{F_R2,0,6,0},{F_R3,0,6,0},{F_P,0,5,0}}},
    {0x03, 1, 0, 10, {10, 10, 10},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,10,0},{F_G1,0,10,0},{F_B1,0,10,0}}},
    {0x07, 1, 1, 11, {9, 9, 9},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,9,0},{F_R0,10,1,0},{F_G1,0,9,0},{F_G0,10,1,0},{F_B1,0,9,0},{F_B0,10,1,0}}},
    {0x0b, 1, 1, 12, {8, 8, 8},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,8,0},{F_R0,10,2,1},{F_G1,0,8,0},{F_G0,10,2,1},{F_B1,0,8,0},{F_B0,10,2,1}}},
    {0x0f, 1, 1, 16, {4, 4, 4},
     {{F_R0,0,10,0},{F_G0,0,10,0},{F_B0,0,10,0},{F_R1,0,4,0},{F_R0,10,6,1},{F_G1,0,4,0},{F_G0,10,6,1},{F_B1,0,4,0},{F_B0,10,6,1}}},
};

int signExtend(int v, int bits) {
    const std::uint32_t m = 1u << (bits - 1);
    const std::uint32_t u = static_cast<std::uint32_t>(v) & ((bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u));
    return static_cast<int>((u ^ m) - m);
}

int bc6Unquantize(int v, int bits, bool isSigned) {
    if (!isSigned) {
        if (bits >= 15) {
            return v;
        }
        if (v == 0) {
            return 0;
        }
        if (v == (1 << bits) - 1) {
            return 0xffff;
        }
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) {
        return v;
    }
    const bool neg = v < 0;
    int m = neg ? -v : v;
    int u;
    if (m == 0) {
        u = 0;
    } else if (m >= (1 << (bits - 1)) - 1) {
        u = 0x7fff;
    } else {
        u = ((m << 15) + 0x4000) >> (bits - 1);
    }
    return neg ? -u : u;
}

std::uint16_t bc6Finish(int v, bool isSigned) {
    if (!isSigned) {
        return static_cast<std::uint16_t>((v * 31) >> 6);
    }
    const int scaled = v < 0 ? -(((-v) * 31) >> 5) : (v * 31) >> 5;
    return scaled < 0 ? static_cast<std::uint16_t>(0x8000 | (-scaled)) : static_cast<std::uint16_t>(scaled);
}

// ---- plain texel decode ------------------------------------------------------------------------------------

float unorm(std::uint32_t v, int bits) { return static_cast<float>(v) / static_cast<float>((1ull << bits) - 1); }

float snorm(std::uint32_t v, int bits) {
    const int s = signExtend(static_cast<int>(v), bits);
    const float f = static_cast<float>(s) / static_cast<float>((1 << (bits - 1)) - 1);
    return std::max(f, -1.0f);
}

float ufloatBits(std::uint32_t v, int mantissaBits) {
    // Unsigned small float with a 5-bit exponent (B10G11R11): bias 15.
    const std::uint32_t e = v >> mantissaBits, m = v & ((1u << mantissaBits) - 1u);
    const float scale = static_cast<float>(1u << mantissaBits);
    if (e == 0) {
        return std::ldexp(static_cast<float>(m) / scale, -14);
    }
    if (e == 31) {
        return m ? std::nanf("") : INFINITY;
    }
    return std::ldexp(1.0f + static_cast<float>(m) / scale, static_cast<int>(e) - 15);
}

bool decodePlain(TexFormat format, const TexFormatInfo& info, const std::uint8_t* t, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (format) {
    case TexFormat::R5G6B5_UNORM_PACK16: {
        const std::uint32_t v = t[0] | (t[1] << 8);
        out[0] = unorm(v >> 11, 5);
        out[1] = unorm((v >> 5) & 63, 6);
        out[2] = unorm(v & 31, 5);
        return true;
    }
    case TexFormat::A1R5G5B5_UNORM_PACK16: {
        const std::uint32_t v = t[0] | (t[1] << 8);
        out[0] = unorm((v >> 10) & 31, 5);
        out[1] = unorm((v >> 5) & 31, 5);
        out[2] = unorm(v & 31, 5);
        out[3] = static_cast<float>(v >> 15);
        return true;
    }
    case TexFormat::A2R10G10B10_UNORM_PACK32: {
        const std::uint32_t v = load32(t);
        out[0] = unorm((v >> 20) & 1023, 10);
        out[1] = unorm((v >> 10) & 1023, 10);
        out[2] = unorm(v & 1023, 10);
        out[3] = unorm(v >> 30, 2);
        return true;
    }
    case TexFormat::A2B10G10R10_UNORM_PACK32:
    case TexFormat::A2B10G10R10_UINT_PACK32: {
        const std::uint32_t v = load32(t);
        const bool u = format == TexFormat::A2B10G10R10_UINT_PACK32;
        const std::uint32_t c[4] = {v & 1023, (v >> 10) & 1023, (v >> 20) & 1023, v >> 30};
        for (int k = 0; k < 4; ++k) {
            out[k] = u ? static_cast<float>(c[k]) : unorm(c[k], k == 3 ? 2 : 10);
        }
        return true;
    }
    case TexFormat::B10G11R11_UFLOAT_PACK32: {
        const std::uint32_t v = load32(t);
        out[0] = ufloatBits(v & 0x7ff, 6);
        out[1] = ufloatBits((v >> 11) & 0x7ff, 6);
        out[2] = ufloatBits(v >> 22, 5);
        return true;
    }
    case TexFormat::E5B9G9R9_UFLOAT_PACK32: {
        const std::uint32_t v = load32(t);
        const int e = static_cast<int>(v >> 27) - 15 - 9;
        out[0] = std::ldexp(static_cast<float>(v & 511), e);
        out[1] = std::ldexp(static_cast<float>((v >> 9) & 511), e);
        out[2] = std::ldexp(static_cast<float>((v >> 18) & 511), e);
        return true;
    }
    default:
        break;
    }
    const bool bgra = format == TexFormat::B8G8R8A8_UNORM || format == TexFormat::B8G8R8A8_SRGB;
    const int compBytes = info.bytesPerBlock / info.channels;
    for (int c = 0; c < info.channels; ++c) {
        const std::uint8_t* p = t + c * compBytes;
        std::uint32_t raw = 0;
        for (int k = 0; k < compBytes; ++k) {
            raw |= std::uint32_t(p[k]) << (8 * k);
        }
        const int bits = compBytes * 8;
        float v = 0.0f;
        switch (info.kind) {
        case TexelKind::Unorm:
            v = unorm(raw, bits);
            break;
        case TexelKind::Snorm:
            v = snorm(raw, bits);
            break;
        case TexelKind::Uint:
            v = static_cast<float>(raw);
            break;
        case TexelKind::Sint:
            v = static_cast<float>(bits == 32 ? static_cast<std::int32_t>(raw) : signExtend(static_cast<int>(raw), bits));
            break;
        case TexelKind::Float:
            if (bits == 16) {
                v = halfToFloat(static_cast<std::uint16_t>(raw));
            } else {
                std::memcpy(&v, &raw, 4);
            }
            break;
        default:
            return false;
        }
        out[bgra && c < 3 ? 2 - c : c] = v;
    }
    return true;
}

} // namespace

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h >> 15) << 31;
    const std::uint32_t e = (h >> 10) & 31, m = h & 1023;
    std::uint32_t bits;
    if (e == 0) {
        if (m == 0) {
            bits = sign;
        } else {
            const float f = std::ldexp(static_cast<float>(m), -24);
            std::memcpy(&bits, &f, 4);
            bits |= sign;
        }
    } else if (e == 31) {
        bits = sign | 0x7f800000u | (m << 13);
    } else {
        bits = sign | ((e + 112) << 23) | (m << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

void decodeBc1Block(const std::uint8_t* b, std::uint8_t out[16][4], bool forceFourColour) {
    const std::uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    std::uint8_t pal[4][4];
    auto rgb565 = [](std::uint32_t c, std::uint8_t* o) {
        o[0] = expand((c >> 11) & 31, 5);
        o[1] = expand((c >> 5) & 63, 6);
        o[2] = expand(c & 31, 5);
        o[3] = 255;
    };
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || forceFourColour) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const std::uint32_t bits = load32(b + 4);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
    }
}

void decodeBc2Block(const std::uint8_t* b, std::uint8_t out[16][4]) {
    decodeBc1Block(b + 8, out, true);
    for (int i = 0; i < 16; ++i) {
        out[i][3] = expand((b[i / 2] >> (4 * (i & 1))) & 15, 4);
    }
}

void decodeBc3Block(const std::uint8_t* b, std::uint8_t out[16][4]) {
    decodeBc1Block(b + 8, out, true);
    const std::uint32_t a0 = b[0], a1 = b[1];
    std::uint8_t pal[8];
    pal[0] = static_cast<std::uint8_t>(a0);
    pal[1] = static_cast<std::uint8_t>(a1);
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((7 - k) * a0 + k * a1 + 3) / 7);
        }
    } else {
        for (int k = 1; k <= 4; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((5 - k) * a0 + k * a1 + 2) / 5);
        }
        pal[6] = 0;
        pal[7] = 255;
    }
    const std::uint64_t bits = load64(b) >> 16;
    for (int i = 0; i < 16; ++i) {
        out[i][3] = pal[(bits >> (3 * i)) & 7];
    }
}

void decodeBc4Block(const std::uint8_t* b, bool isSigned, float out[16]) {
    float e[8];
    if (isSigned) {
        e[0] = std::max(static_cast<float>(static_cast<std::int8_t>(b[0])) / 127.0f, -1.0f);
        e[1] = std::max(static_cast<float>(static_cast<std::int8_t>(b[1])) / 127.0f, -1.0f);
    } else {
        e[0] = static_cast<float>(b[0]) / 255.0f;
        e[1] = static_cast<float>(b[1]) / 255.0f;
    }
    if (e[0] > e[1]) {
        for (int k = 1; k <= 6; ++k) {
            e[1 + k] = (static_cast<float>(7 - k) * e[0] + static_cast<float>(k) * e[1]) / 7.0f;
        }
    } else {
        for (int k = 1; k <= 4; ++k) {
            e[1 + k] = (static_cast<float>(5 - k) * e[0] + static_cast<float>(k) * e[1]) / 5.0f;
        }
        e[6] = isSigned ? -1.0f : 0.0f;
        e[7] = 1.0f;
    }
    const std::uint64_t bits = load64(b) >> 16;
    for (int i = 0; i < 16; ++i) {
        out[i] = e[(bits >> (3 * i)) & 7];
    }
}

void decodeBc6hBlock(const std::uint8_t* block, bool isSigned, std::uint16_t out[16][3]) {
    BitReader br(block);
    std::uint32_t code = br.read(2);
    if (code > 1) {
        code |= br.read(3) << 2;
    }
    const Bc6Mode* mode = nullptr;
    for (const Bc6Mode& m : kBc6Modes) {
        if (m.code == code) {
            mode = &m;
        }
    }
    if (!mode) { // reserved mode: all channels zero
        std::memset(out, 0, sizeof(std::uint16_t) * 16 * 3);
        return;
    }
    int f[13] = {};
    for (const Bits& b : mode->layout) {
        if (b.count == 0) {
            break;
        }
        std::uint32_t v = br.read(b.count);
        if (b.reversed) {
            std::uint32_t r = 0;
            for (int k = 0; k < b.count; ++k) {
                r = (r << 1) | ((v >> k) & 1u);
            }
            v = r;
        }
        f[b.field] |= static_cast<int>(v << b.shift);
    }
    const int numEndpoints = mode->subsets * 2;
    const int ep = mode->epBits;
    int e[4][3];
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            e[i][c] = f[i * 3 + c];
        }
    }
    if (isSigned) {
        for (int c = 0; c < 3; ++c) {
            e[0][c] = signExtend(e[0][c], ep);
        }
    }
    if (mode->transformed || isSigned) {
        for (int i = 1; i < numEndpoints; ++i) {
            for (int c = 0; c < 3; ++c) {
                e[i][c] = signExtend(e[i][c], mode->deltaBits[c]);
            }
        }
    }
    if (mode->transformed) {
        for (int i = 1; i < numEndpoints; ++i) {
            for (int c = 0; c < 3; ++c) {
                e[i][c] = (e[i][c] + e[0][c]) & ((1 << ep) - 1);
                if (isSigned) {
                    e[i][c] = signExtend(e[i][c], ep);
                }
            }
        }
    }
    for (int i = 0; i < numEndpoints; ++i) {
        for (int c = 0; c < 3; ++c) {
            e[i][c] = bc6Unquantize(e[i][c], ep, isSigned);
        }
    }
    const int partition = f[F_P];
    const int indexBits = mode->subsets == 1 ? 4 : 3;
    const int* w = weightTable(indexBits);
    for (int t = 0; t < 16; ++t) {
        const int subset = static_cast<int>(subsetOf(mode->subsets, partition, t));
        const int bits = isAnchor(mode->subsets, partition, t) ? indexBits - 1 : indexBits;
        const int index = static_cast<int>(br.read(bits));
        for (int c = 0; c < 3; ++c) {
            out[t][c] = bc6Finish(interpolate(e[2 * subset][c], e[2 * subset + 1][c], w, index), isSigned);
        }
    }
}

void decodeBc7Block(const std::uint8_t* block, std::uint8_t out[16][4]) {
    // mode: NS, PB, RB, ISB, CB, AB, EPB, SPB, IB, IB2
    static constexpr std::uint8_t kModes[8][10] = {
        {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
        {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
        {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
    };
    int mode = 0;
    while (mode < 8 && !((block[0] >> mode) & 1)) {
        ++mode;
    }
    if (mode == 8) {
        std::memset(out, 0, 16 * 4);
        return;
    }
    const std::uint8_t* m = kModes[mode];
    const int ns = m[0], cb = m[4], ab = m[5], ib = m[8], ib2 = m[9];
    BitReader br(block);
    br.read(mode + 1);
    const int partition = static_cast<int>(br.read(m[1]));
    const int rotation = static_cast<int>(br.read(m[2]));
    const int indexSelection = static_cast<int>(br.read(m[3]));
    int e[6][4] = {};
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < ns * 2; ++i) {
            e[i][c] = static_cast<int>(br.read(cb));
        }
    }
    for (int i = 0; i < ns * 2; ++i) {
        e[i][3] = ab ? static_cast<int>(br.read(ab)) : 0;
    }
    int cbits = cb, abits = ab;
    if (m[6]) { // unique p-bit per endpoint
        for (int i = 0; i < ns * 2; ++i) {
            const int p = static_cast<int>(br.read(1));
            for (int c = 0; c < 4; ++c) {
                e[i][c] = (e[i][c] << 1) | p;
            }
        }
        ++cbits;
        abits = ab ? ab + 1 : 0;
    } else if (m[7]) { // shared p-bit per subset
        for (int s = 0; s < ns; ++s) {
            const int p = static_cast<int>(br.read(1));
            for (int k = 0; k < 2; ++k) {
                for (int c = 0; c < 3; ++c) {
                    e[2 * s + k][c] = (e[2 * s + k][c] << 1) | p;
                }
            }
        }
        ++cbits;
    }
    for (int i = 0; i < ns * 2; ++i) {
        for (int c = 0; c < 3; ++c) {
            e[i][c] = (e[i][c] << (8 - cbits)) | (e[i][c] >> (2 * cbits - 8));
        }
        e[i][3] = abits ? ((e[i][3] << (8 - abits)) | (e[i][3] >> (2 * abits - 8))) : 255;
    }
    int idx[16], idx2[16] = {};
    for (int t = 0; t < 16; ++t) {
        idx[t] = static_cast<int>(br.read(isAnchor(ns, partition, t) ? ib - 1 : ib));
    }
    if (ib2) {
        for (int t = 0; t < 16; ++t) {
            idx2[t] = static_cast<int>(br.read(t == 0 ? ib2 - 1 : ib2));
        }
    }
    for (int t = 0; t < 16; ++t) {
        const int s = static_cast<int>(subsetOf(ns, partition, t));
        const int* e0 = e[2 * s];
        const int* e1 = e[2 * s + 1];
        int px[4];
        if (!ib2) {
            const int* w = weightTable(ib);
            for (int c = 0; c < 4; ++c) {
                px[c] = interpolate(e0[c], e1[c], w, idx[t]);
            }
        } else {
            const bool swap = indexSelection != 0;
            const int ci = swap ? idx2[t] : idx[t], ai = swap ? idx[t] : idx2[t];
            const int* wc = weightTable(swap ? ib2 : ib);
            const int* wa = weightTable(swap ? ib : ib2);
            for (int c = 0; c < 3; ++c) {
                px[c] = interpolate(e0[c], e1[c], wc, ci);
            }
            px[3] = interpolate(e0[3], e1[3], wa, ai);
        }
        if (rotation) {
            std::swap(px[3], px[rotation - 1]);
        }
        for (int c = 0; c < 4; ++c) {
            out[t][c] = static_cast<std::uint8_t>(px[c]);
        }
    }
}

std::optional<std::vector<float>> decodeToRgba32f(TexFormat format, std::uint32_t width, std::uint32_t height,
                                                  std::uint32_t depth, std::span<const std::uint8_t> data) {
    const TexFormatInfo* info = texFormatInfo(format);
    const std::uint64_t size = texLevelSize(format, width, height, depth);
    if (!info || size == 0 || data.size() < size) {
        return std::nullopt;
    }
    const std::uint64_t texels = std::uint64_t(width) * height * depth;
    if (texels > (std::uint64_t(1) << 28)) {
        return std::nullopt; // 4 GiB of floats: not a CPU decode job
    }
    std::vector<float> out(static_cast<std::size_t>(texels * 4));
    auto px = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return out.data() + ((std::size_t(z) * height + y) * width + x) * 4;
    };
    if (info->kind != TexelKind::Block) {
        const std::uint64_t row = texRowPitch(format, width);
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint8_t* r = data.data() + (std::uint64_t(z) * height + y) * row;
                for (std::uint32_t x = 0; x < width; ++x) {
                    decodePlain(format, *info, r + std::uint64_t(x) * info->bytesPerBlock, px(x, y, z));
                }
            }
        }
        return out;
    }
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const std::uint8_t* blk = data.data();
    for (std::uint32_t z = 0; z < depth; ++z) {
        for (std::uint32_t by = 0; by < bh; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx, blk += info->bytesPerBlock) {
                float texel[16][4];
                std::uint8_t u8[16][4];
                switch (format) {
                case TexFormat::BC1_RGB_UNORM_BLOCK:
                case TexFormat::BC1_RGB_SRGB_BLOCK:
                case TexFormat::BC1_RGBA_UNORM_BLOCK:
                case TexFormat::BC1_RGBA_SRGB_BLOCK:
                case TexFormat::BC2_UNORM_BLOCK:
                case TexFormat::BC2_SRGB_BLOCK:
                case TexFormat::BC3_UNORM_BLOCK:
                case TexFormat::BC3_SRGB_BLOCK:
                case TexFormat::BC7_UNORM_BLOCK:
                case TexFormat::BC7_SRGB_BLOCK: {
                    if (format == TexFormat::BC2_UNORM_BLOCK || format == TexFormat::BC2_SRGB_BLOCK) {
                        decodeBc2Block(blk, u8);
                    } else if (format == TexFormat::BC3_UNORM_BLOCK || format == TexFormat::BC3_SRGB_BLOCK) {
                        decodeBc3Block(blk, u8);
                    } else if (format == TexFormat::BC7_UNORM_BLOCK || format == TexFormat::BC7_SRGB_BLOCK) {
                        decodeBc7Block(blk, u8);
                    } else {
                        decodeBc1Block(blk, u8, false);
                    }
                    const bool opaque = format == TexFormat::BC1_RGB_UNORM_BLOCK || format == TexFormat::BC1_RGB_SRGB_BLOCK;
                    for (int i = 0; i < 16; ++i) {
                        for (int c = 0; c < 4; ++c) {
                            texel[i][c] = static_cast<float>(u8[i][c]) / 255.0f;
                        }
                        if (opaque) {
                            texel[i][3] = 1.0f;
                        }
                    }
                    break;
                }
                case TexFormat::BC4_UNORM_BLOCK:
                case TexFormat::BC4_SNORM_BLOCK:
                case TexFormat::BC5_UNORM_BLOCK:
                case TexFormat::BC5_SNORM_BLOCK: {
                    const bool sgn = format == TexFormat::BC4_SNORM_BLOCK || format == TexFormat::BC5_SNORM_BLOCK;
                    const bool two = format == TexFormat::BC5_UNORM_BLOCK || format == TexFormat::BC5_SNORM_BLOCK;
                    float r[16], g[16] = {};
                    decodeBc4Block(blk, sgn, r);
                    if (two) {
                        decodeBc4Block(blk + 8, sgn, g);
                    }
                    for (int i = 0; i < 16; ++i) {
                        texel[i][0] = r[i];
                        texel[i][1] = g[i];
                        texel[i][2] = 0.0f;
                        texel[i][3] = 1.0f;
                    }
                    break;
                }
                case TexFormat::BC6H_UFLOAT_BLOCK:
                case TexFormat::BC6H_SFLOAT_BLOCK: {
                    std::uint16_t h[16][3];
                    decodeBc6hBlock(blk, format == TexFormat::BC6H_SFLOAT_BLOCK, h);
                    for (int i = 0; i < 16; ++i) {
                        for (int c = 0; c < 3; ++c) {
                            texel[i][c] = halfToFloat(h[i][c]);
                        }
                        texel[i][3] = 1.0f;
                    }
                    break;
                }
                default:
                    return std::nullopt;
                }
                for (int i = 0; i < 16; ++i) {
                    const std::uint32_t x = bx * 4 + std::uint32_t(i % 4), y = by * 4 + std::uint32_t(i / 4);
                    if (x < width && y < height) {
                        std::memcpy(px(x, y, z), texel[i], sizeof(float) * 4);
                    }
                }
            }
        }
    }
    return out;
}

std::optional<std::vector<float>> decodeSubresource(const TextureImage& image, const Subresource& s) {
    if (s.offset > image.data.size() || s.size > image.data.size() - s.offset) {
        return std::nullopt;
    }
    return decodeToRgba32f(image.format, s.width, s.height, s.depth,
                           std::span<const std::uint8_t>(image.data).subspan(static_cast<std::size_t>(s.offset),
                                                                             static_cast<std::size_t>(s.size)));
}

void applySwizzle(std::span<float> rgba, Swizzle swizzle) {
    if (swizzle.identity()) {
        return;
    }
    auto pick = [](const float* t, Swz s) {
        switch (s) {
        case Swz::R:
            return t[0];
        case Swz::G:
            return t[1];
        case Swz::B:
            return t[2];
        case Swz::A:
            return t[3];
        case Swz::Zero:
            return 0.0f;
        case Swz::One:
            return 1.0f;
        }
        return 0.0f;
    };
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const float t[4] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
        rgba[i] = pick(t, swizzle.r);
        rgba[i + 1] = pick(t, swizzle.g);
        rgba[i + 2] = pick(t, swizzle.b);
        rgba[i + 3] = pick(t, swizzle.a);
    }
}

} // namespace fuse::relight::mods::assets
