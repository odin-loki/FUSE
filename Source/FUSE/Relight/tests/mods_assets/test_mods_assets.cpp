// FUSE Relight RL-3.3: unit tests of the mod asset readers (ctest rl_mods_assets_unit).
//
//   formats    format table, level sizes, sRGB twins, the §4.6 colour-space rule;
//   decode     "decode vs expected" for every TexFormat: known texels for every uncompressed format, known
//              blocks for BC1-BC7, and the bcdec oracle digests over every BC7 mode, every BC6H mode (signed
//              and unsigned, reserved modes) and BC4/BC5 (bc_oracle.hpp);
//   dds        DX10 write/read round trip of every format with a DXGI spelling (2D mips, arrays, cubes,
//              volumes, 1D); every DX9 FourCC and bit-mask format (mapping, swizzle, conversion, decoded
//              texel); DX9 cubes and volumes; the RL-1.8 capture writer's DX9 files for every D3DFORMAT it
//              writes read back byte-identical and decode identically to its own decodeRgba8; malformed
//              headers are rejected;
//   gdeflate   reference round trip (the vendored GDeflate::Compress -> FUSE's hardened decoder and the
//              reference GDeflate::Decompress) over edge sizes, patterns and levels 1..12; stream
//              inspection; corrupted streams fail cleanly;
//   package    `.pkg` layout facts from rtx_asset_package.h (16-byte header, 20-byte AssetDesc, 16-byte
//              BlobDesc with the 40/8/8 bitfield), writer -> reader round trips of 2D mip tails, cube arrays,
//              volumes, 1D, buffers, raw and GDeflate blobs, file-backed open(), upstream's blob addressing,
//              names, CRC, and malformed packages.
#include "bc_oracle.hpp"

#include <fuse/relight/mods/assets/asset_package.hpp>
#include <fuse/relight/mods/assets/dds.hpp>
#include <fuse/relight/mods/assets/gdeflate.hpp>
#include <fuse/relight/mods/assets/texture_decode.hpp>
#include <fuse/relight/mods/assets/texture_format.hpp>

#if FUSE_RL_MODS_HAVE_CAPTURE_EXPORT
#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/hash/texture_hash.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                   \
            std::fprintf(stderr, __VA_ARGS__);                                                     \
            std::fprintf(stderr, "\n");                                                            \
        }                                                                                          \
    } while (0)

namespace ma = fuse::relight::mods::assets;
namespace gd = fuse::relight::mods::assets::gdeflate;
namespace rh = fuse::relight::hash;
using ma::Swz;
using ma::TexFormat;
using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(Bytes& o, std::uint32_t v) {
    put16(o, v & 0xffff);
    put16(o, v >> 16);
}
void putF(Bytes& o, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(o, v);
}

Bytes randomBytes(std::size_t n, std::uint64_t seed) {
    rl_bc_oracle::Lcg g{seed};
    Bytes b(n);
    for (auto& x : b) {
        x = static_cast<std::uint8_t>(g.next() >> 11);
    }
    return b;
}

bool near(float a, float b, float tol = 1e-6f) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b);
    }
    return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b));
}

bool texelIs(const std::vector<float>& px, std::size_t texel, const float expect[4], float tol = 1e-6f) {
    for (int c = 0; c < 4; ++c) {
        if (!near(px[texel * 4 + c], expect[c], tol)) {
            std::fprintf(stderr, "    texel %zu channel %d: got %.9g expected %.9g\n", texel, c, px[texel * 4 + c], expect[c]);
            return false;
        }
    }
    return true;
}

/// Minimal LSB-first bit writer for hand-built blocks.
struct BitWriter {
    std::uint8_t b[16] = {};
    int pos = 0;
    void put(std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++pos) {
            if ((v >> i) & 1u) {
                b[pos / 8] = static_cast<std::uint8_t>(b[pos / 8] | (1u << (pos % 8)));
            }
        }
    }
};

// ---- formats ------------------------------------------------------------------------------------------------

void testFormats() {
    CHECK(ma::texFormatInfo(TexFormat::BC7_SRGB_BLOCK)->bytesPerBlock == 16);
    CHECK(ma::texFormatInfo(TexFormat::BC1_RGB_UNORM_BLOCK)->bytesPerBlock == 8);
    CHECK(ma::texFormatInfo(TexFormat::Undefined) == nullptr);
    CHECK(ma::texFormatInfo(static_cast<TexFormat>(999)) == nullptr);
    CHECK(ma::texLevelSize(TexFormat::BC1_RGBA_UNORM_BLOCK, 5, 5, 1) == 4 * 8);
    CHECK(ma::texLevelSize(TexFormat::BC7_UNORM_BLOCK, 1, 1, 1) == 16);
    CHECK(ma::texLevelSize(TexFormat::R32G32B32_SFLOAT, 3, 2, 2) == 3 * 12 * 2 * 2);
    CHECK(ma::texLevelSize(TexFormat::R8_UNORM, 0, 1, 1) == 0);
    CHECK(ma::texLevelSize(TexFormat::R32G32B32A32_SFLOAT, 0xffffffffu, 0xffffffffu, 0xffffffffu) == 0); // overflow
    CHECK(ma::fullMipCount(1, 1, 1) == 1);
    CHECK(ma::fullMipCount(256, 3, 1) == 9);
    CHECK(ma::fullMipCount(4, 4, 32) == 6);
    CHECK(ma::mipExtent(5, 1) == 2 && ma::mipExtent(5, 3) == 1 && ma::mipExtent(5, 40) == 1);
    CHECK(ma::srgbVariant(TexFormat::BC3_UNORM_BLOCK) == TexFormat::BC3_SRGB_BLOCK);
    CHECK(ma::linearVariant(TexFormat::B8G8R8A8_SRGB) == TexFormat::B8G8R8A8_UNORM);
    CHECK(ma::srgbVariant(TexFormat::BC5_UNORM_BLOCK) == TexFormat::BC5_UNORM_BLOCK);
    CHECK(!ma::hasSrgbVariant(TexFormat::BC6H_UFLOAT_BLOCK) && ma::hasSrgbVariant(TexFormat::BC7_SRGB_BLOCK));

    // §4.6: the USD colour-space hint wins; "auto" follows the Remix parameter (colour maps sRGB, data linear).
    using H = ma::ColourSpaceHint;
    CHECK(ma::colourSpaceFromUsd("sRGB") == H::Srgb && ma::colourSpaceFromUsd("raw") == H::Linear &&
          ma::colourSpaceFromUsd("auto") == H::Auto && ma::colourSpaceFromUsd("") == H::Auto);
    CHECK(ma::remixParameterIsSrgb("diffuse_texture") && ma::remixParameterIsSrgb("inputs:emissive_mask_texture"));
    CHECK(!ma::remixParameterIsSrgb("normalmap_texture") && !ma::remixParameterIsSrgb("reflectionroughness_texture"));
    CHECK(ma::resolveColourSpace(TexFormat::BC7_UNORM_BLOCK, H::Auto, "diffuse_texture") == TexFormat::BC7_SRGB_BLOCK);
    CHECK(ma::resolveColourSpace(TexFormat::BC7_SRGB_BLOCK, H::Auto, "normalmap_texture") == TexFormat::BC7_UNORM_BLOCK);
    CHECK(ma::resolveColourSpace(TexFormat::BC1_RGBA_UNORM_BLOCK, H::Srgb, "normalmap_texture") == TexFormat::BC1_RGBA_SRGB_BLOCK);
    CHECK(ma::resolveColourSpace(TexFormat::R8G8B8A8_SRGB, H::Linear, "diffuse_texture") == TexFormat::R8G8B8A8_UNORM);
    CHECK(ma::resolveColourSpace(TexFormat::R8G8B8A8_SRGB, H::Auto, "") == TexFormat::R8G8B8A8_SRGB);
    CHECK(ma::resolveColourSpace(TexFormat::BC5_UNORM_BLOCK, H::Srgb, "diffuse_texture") == TexFormat::BC5_UNORM_BLOCK);
}

// ---- decode vs expected ---------------------------------------------------------------------------------------

struct PlainCase {
    TexFormat format;
    Bytes texel;
    float expect[4];
};

std::uint16_t f2h(float f) { // exact for the values used below
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    const std::uint32_t sign = (v >> 16) & 0x8000u;
    const int e = int((v >> 23) & 0xff) - 127 + 15;
    return static_cast<std::uint16_t>(sign | (std::uint32_t(e) << 10) | ((v >> 13) & 0x3ffu));
}

Bytes b16(std::initializer_list<std::uint32_t> vals) {
    Bytes o;
    for (auto v : vals) {
        put16(o, v);
    }
    return o;
}
Bytes b32(std::initializer_list<std::uint32_t> vals) {
    Bytes o;
    for (auto v : vals) {
        put32(o, v);
    }
    return o;
}
Bytes bf(std::initializer_list<float> vals) {
    Bytes o;
    for (auto v : vals) {
        putF(o, v);
    }
    return o;
}

void testDecodePlain() {
    const float n = -1.0f;
    const std::vector<PlainCase> cases = {
        {TexFormat::R5G6B5_UNORM_PACK16, b16({(10u << 11) | (20u << 5) | 5u}), {10 / 31.f, 20 / 63.f, 5 / 31.f, 1}},
        {TexFormat::A1R5G5B5_UNORM_PACK16, b16({0x8000u | (3u << 10) | (7u << 5) | 31u}), {3 / 31.f, 7 / 31.f, 1, 1}},
        {TexFormat::R8_UNORM, {128}, {128 / 255.f, 0, 0, 1}},
        {TexFormat::R8_SNORM, {0x80}, {n, 0, 0, 1}},
        {TexFormat::R8_UINT, {200}, {200, 0, 0, 1}},
        {TexFormat::R8_SINT, {0xff}, {n, 0, 0, 1}},
        {TexFormat::R8G8_UNORM, {0, 255}, {0, 1, 0, 1}},
        {TexFormat::R8G8_SNORM, {0x81, 0x40}, {n, 64 / 127.f, 0, 1}},
        {TexFormat::R8G8_UINT, {1, 2}, {1, 2, 0, 1}},
        {TexFormat::R8G8_SINT, {0xfe, 3}, {-2, 3, 0, 1}},
        {TexFormat::R8G8B8A8_UNORM, {255, 0, 51, 102}, {1, 0, 0.2f, 0.4f}},
        {TexFormat::R8G8B8A8_SRGB, {255, 0, 51, 102}, {1, 0, 0.2f, 0.4f}},
        {TexFormat::R8G8B8A8_SNORM, {127, 0x81, 0, 0x80}, {1, n, 0, n}},
        {TexFormat::R8G8B8A8_UINT, {1, 2, 3, 4}, {1, 2, 3, 4}},
        {TexFormat::R8G8B8A8_SINT, {0xff, 0xfe, 0, 5}, {-1, -2, 0, 5}},
        {TexFormat::B8G8R8A8_UNORM, {51, 0, 255, 102}, {1, 0, 0.2f, 0.4f}},
        {TexFormat::B8G8R8A8_SRGB, {51, 0, 255, 102}, {1, 0, 0.2f, 0.4f}},
        {TexFormat::A2R10G10B10_UNORM_PACK32, b32({(2u << 30) | (1023u << 20) | (512u << 10)}), {1, 512 / 1023.f, 0, 2 / 3.f}},
        {TexFormat::A2B10G10R10_UNORM_PACK32, b32({(1u << 30) | (1023u << 10) | 256u}), {256 / 1023.f, 1, 0, 1 / 3.f}},
        {TexFormat::A2B10G10R10_UINT_PACK32, b32({(1u << 30) | (1023u << 10) | 256u}), {256, 1023, 0, 1}},
        {TexFormat::R16_UNORM, b16({0xffff}), {1, 0, 0, 1}},
        {TexFormat::R16_SNORM, b16({0x8000}), {n, 0, 0, 1}},
        {TexFormat::R16_UINT, b16({0x1234}), {4660, 0, 0, 1}},
        {TexFormat::R16_SINT, b16({0xffff}), {n, 0, 0, 1}},
        {TexFormat::R16_SFLOAT, b16({f2h(-2.0f)}), {-2, 0, 0, 1}},
        {TexFormat::R16G16_UNORM, b16({0, 0x8000}), {0, 32768 / 65535.f, 0, 1}},
        {TexFormat::R16G16_SNORM, b16({0x7fff, 0x8001}), {1, n, 0, 1}},
        {TexFormat::R16G16_UINT, b16({7, 65535}), {7, 65535, 0, 1}},
        {TexFormat::R16G16_SINT, b16({0x8000, 9}), {-32768, 9, 0, 1}},
        {TexFormat::R16G16_SFLOAT, b16({f2h(0.5f), f2h(1024.0f)}), {0.5f, 1024, 0, 1}},
        {TexFormat::R16G16B16A16_UNORM, b16({0, 0xffff, 0x8000, 0x4000}), {0, 1, 32768 / 65535.f, 16384 / 65535.f}},
        {TexFormat::R16G16B16A16_SNORM, b16({0x7fff, 0x8000, 0, 0x4000}), {1, n, 0, 16384 / 32767.f}},
        {TexFormat::R16G16B16A16_UINT, b16({1, 2, 3, 4}), {1, 2, 3, 4}},
        {TexFormat::R16G16B16A16_SINT, b16({0xffff, 2, 0x8000, 4}), {-1, 2, -32768, 4}},
        {TexFormat::R16G16B16A16_SFLOAT, b16({f2h(0.5f), 0x3c00, f2h(-4.0f), 0x0001}), {0.5f, 1, -4, 5.9604644775390625e-8f}},
        {TexFormat::R32_UINT, b32({123456}), {123456, 0, 0, 1}},
        {TexFormat::R32_SINT, b32({0xfffffff9u}), {-7, 0, 0, 1}},
        {TexFormat::R32_SFLOAT, bf({3.5f}), {3.5f, 0, 0, 1}},
        {TexFormat::R32G32_UINT, b32({1, 2}), {1, 2, 0, 1}},
        {TexFormat::R32G32_SINT, b32({0xffffffffu, 2}), {-1, 2, 0, 1}},
        {TexFormat::R32G32_SFLOAT, bf({-0.25f, 1e20f}), {-0.25f, 1e20f, 0, 1}},
        {TexFormat::R32G32B32_UINT, b32({1, 2, 3}), {1, 2, 3, 1}},
        {TexFormat::R32G32B32_SINT, b32({1, 0xfffffffeu, 3}), {1, -2, 3, 1}},
        {TexFormat::R32G32B32_SFLOAT, bf({1, 2, 3}), {1, 2, 3, 1}},
        {TexFormat::R32G32B32A32_UINT, b32({1, 2, 3, 4}), {1, 2, 3, 4}},
        {TexFormat::R32G32B32A32_SINT, b32({1, 2, 3, 0xfffffffcu}), {1, 2, 3, -4}},
        {TexFormat::R32G32B32A32_SFLOAT, bf({0.1f, 0.2f, 0.3f, 0.4f}), {0.1f, 0.2f, 0.3f, 0.4f}},
        {TexFormat::B10G11R11_UFLOAT_PACK32, b32({(15u << 6) | (((16u << 6) | 32u) << 11) | (((14u << 5) | 16u) << 22)}),
         {1.0f, 3.0f, 0.75f, 1}},
        {TexFormat::E5B9G9R9_UFLOAT_PACK32, b32({256u | (128u << 9) | (384u << 18) | (16u << 27)}), {1.0f, 0.5f, 1.5f, 1}},
    };
    for (const PlainCase& c : cases) {
        auto px = ma::decodeToRgba32f(c.format, 1, 1, 1, c.texel);
        CHECK_MSG(px && texelIs(*px, 0, c.expect), "%s", ma::texFormatInfo(c.format)->name);
    }
    // Every non-block format is covered by the table above.
    int covered = 0, plain = 0;
    for (std::uint32_t v = 0; v < 200; ++v) {
        const ma::TexFormatInfo* info = ma::texFormatInfo(static_cast<TexFormat>(v));
        if (!info || info->kind == ma::TexelKind::Block) {
            continue;
        }
        ++plain;
        for (const PlainCase& c : cases) {
            covered += c.format == static_cast<TexFormat>(v) ? 1 : 0;
        }
    }
    CHECK_MSG(covered == plain, "%d of %d plain formats have a decode case", covered, plain);

    // Row-major layout, depth slices, short data.
    const Bytes rgba = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto img = ma::decodeToRgba32f(TexFormat::R8G8B8A8_UINT, 2, 1, 2, rgba);
    const float t3[4] = {13, 14, 15, 16};
    CHECK(img && img->size() == 16 && texelIs(*img, 3, t3));
    CHECK(!ma::decodeToRgba32f(TexFormat::R8G8B8A8_UINT, 2, 2, 2, rgba));
    CHECK(!ma::decodeToRgba32f(TexFormat::Undefined, 1, 1, 1, rgba));

    // Swizzles (L8 -> rrr1, A8 -> 000r, X8R8G8B8 -> a = 1).
    std::vector<float> t = {0.25f, 0.5f, 0.75f, 0.1f};
    ma::applySwizzle(t, ma::Swizzle{Swz::R, Swz::R, Swz::R, Swz::One});
    const float l[4] = {0.25f, 0.25f, 0.25f, 1};
    CHECK(texelIs(t, 0, l));
    t = {0.25f, 0.5f, 0.75f, 0.1f};
    ma::applySwizzle(t, ma::Swizzle{Swz::Zero, Swz::Zero, Swz::Zero, Swz::R});
    const float a[4] = {0, 0, 0, 0.25f};
    CHECK(texelIs(t, 0, a));
    CHECK(near(ma::halfToFloat(0x7bff), 65504.0f) && std::isinf(ma::halfToFloat(0x7c00)) && std::isnan(ma::halfToFloat(0x7e00)) &&
          ma::halfToFloat(0x8000) == 0.0f && std::signbit(ma::halfToFloat(0x8000)));
}

void testDecodeBlocks() {
    // BC1, four-colour mode (c0 > c1): red, blue and the two thirds.
    const Bytes bc1 = {0x00, 0xf8, 0x1f, 0x00, 0xe4, 0xe4, 0xe4, 0xe4};
    auto px = ma::decodeToRgba32f(TexFormat::BC1_RGBA_UNORM_BLOCK, 4, 4, 1, bc1);
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, third[4] = {170 / 255.f, 0, 85 / 255.f, 1},
                twoThirds[4] = {85 / 255.f, 0, 170 / 255.f, 1};
    CHECK(px && texelIs(*px, 0, red) && texelIs(*px, 1, blue) && texelIs(*px, 2, third) && texelIs(*px, 3, twoThirds));
    // BC1 three-colour mode (c0 <= c1): midpoint and transparent black; BC1_RGB forces alpha to 1.
    const Bytes bc1t = {0x1f, 0x00, 0x00, 0xf8, 0xe4, 0xe4, 0xe4, 0xe4};
    px = ma::decodeToRgba32f(TexFormat::BC1_RGBA_UNORM_BLOCK, 4, 4, 1, bc1t);
    const float mid[4] = {127 / 255.f, 0, 127 / 255.f, 1}, clear[4] = {0, 0, 0, 0}, black[4] = {0, 0, 0, 1};
    CHECK(px && texelIs(*px, 2, mid) && texelIs(*px, 3, clear));
    px = ma::decodeToRgba32f(TexFormat::BC1_RGB_UNORM_BLOCK, 4, 4, 1, bc1t);
    CHECK(px && texelIs(*px, 3, black));
    // BC2: explicit 4-bit alpha (texel i has alpha nibble i).
    Bytes bc2 = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    bc2.insert(bc2.end(), bc1.begin(), bc1.end());
    px = ma::decodeToRgba32f(TexFormat::BC2_UNORM_BLOCK, 4, 4, 1, bc2);
    const float t5[4] = {0, 0, 1, 5 * 17 / 255.f};
    CHECK(px && (*px)[3] == 0.0f && (*px)[15 * 4 + 3] == 1.0f && texelIs(*px, 5, t5));
    // BC3: eight-value alpha palette (a0 = 255 > a1 = 0), indices 0..7 on texels 0..7.
    Bytes bc3 = {255, 0};
    std::uint64_t idx = 0;
    for (int i = 0; i < 8; ++i) {
        idx |= std::uint64_t(i) << (3 * i);
    }
    for (int k = 0; k < 6; ++k) {
        bc3.push_back(static_cast<std::uint8_t>(idx >> (8 * k)));
    }
    bc3.insert(bc3.end(), bc1.begin(), bc1.end());
    px = ma::decodeToRgba32f(TexFormat::BC3_UNORM_BLOCK, 4, 4, 1, bc3);
    CHECK(px && (*px)[3] == 1.0f && (*px)[4 + 3] == 0.0f && near((*px)[2 * 4 + 3], ((6 * 255 + 3) / 7) / 255.f));
    // BC4 / BC5: float interpolation; BC5 SNORM endpoints 127 / -127.
    Bytes bc4 = {255, 0};
    for (int k = 0; k < 6; ++k) {
        bc4.push_back(static_cast<std::uint8_t>(idx >> (8 * k)));
    }
    px = ma::decodeToRgba32f(TexFormat::BC4_UNORM_BLOCK, 4, 4, 1, bc4);
    const float six7[4] = {6.0f / 7.0f, 0, 0, 1};
    CHECK(px && texelIs(*px, 2, six7, 1e-6f));
    Bytes bc5 = {127, 0x81, 0x01, 0, 0, 0, 0, 0, 127, 0, 0, 0, 0, 0, 0, 0};
    px = ma::decodeToRgba32f(TexFormat::BC5_SNORM_BLOCK, 4, 4, 1, bc5);
    const float t0[4] = {-1, 1, 0, 1}, t1[4] = {1, 1, 0, 1};
    CHECK(px && texelIs(*px, 0, t0) && texelIs(*px, 1, t1));
    // BC6H mode 11 (10.10 raw endpoints): e0 = 0, e1 = 1023 -> index 0 is 0, index 15 the largest half.
    {
        BitWriter w;
        w.put(0x03, 5);
        for (int c = 0; c < 3; ++c) {
            w.put(0, 10);
        }
        for (int c = 0; c < 3; ++c) {
            w.put(1023, 10);
        }
        w.put(0, 3); // texel 0 (anchor, 3 bits)
        for (int t = 1; t < 16; ++t) {
            w.put(15, 4);
        }
        std::uint16_t h[16][3];
        ma::decodeBc6hBlock(w.b, false, h);
        CHECK(h[0][0] == 0 && h[1][0] == 0x7bff && h[15][2] == 0x7bff);
        const Bytes blk(w.b, w.b + 16);
        px = ma::decodeToRgba32f(TexFormat::BC6H_UFLOAT_BLOCK, 4, 4, 1, blk);
        const float hdr[4] = {65504, 65504, 65504, 1};
        CHECK(px && texelIs(*px, 15, hdr));
    }
    // BC7 mode 6: endpoints 0 and 127 with p-bits 0 / 1 -> 0 and 255; texel i uses index i.
    {
        BitWriter w;
        w.put(1u << 6, 7);
        for (int c = 0; c < 4; ++c) {
            w.put(0, 7);
            w.put(127, 7);
        }
        w.put(0, 1);
        w.put(1, 1);
        w.put(0, 3);
        for (int t = 1; t < 16; ++t) {
            w.put(static_cast<std::uint32_t>(t), 4);
        }
        std::uint8_t out[16][4];
        ma::decodeBc7Block(w.b, out);
        bool ok = true;
        for (int t = 0; t < 16; ++t) {
            const int wts[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
            const int e = (255 * wts[t] + 32) >> 6;
            ok = ok && out[t][0] == e && out[t][3] == e;
        }
        CHECK(ok);
    }

    // The bcdec oracle over every mode.
    using namespace rl_bc_oracle;
    {
        const Bytes blocks = bc7Blocks();
        Fnv f;
        for (std::size_t i = 0; i < blocks.size(); i += 16) {
            std::uint8_t out[16][4];
            ma::decodeBc7Block(&blocks[i], out);
            f.bytes(out, 64);
        }
        CHECK_MSG(f.h == kBc7Digest, "BC7 digest %016llx", static_cast<unsigned long long>(f.h));
        // The same blocks through the image path (BC7_SRGB decodes the stored values).
        auto img = ma::decodeToRgba32f(TexFormat::BC7_SRGB_BLOCK, 4, static_cast<std::uint32_t>(blocks.size() / 16 * 4), 1, blocks);
        CHECK(img.has_value());
    }
    {
        const Bytes blocks = bc6Blocks();
        for (int s = 0; s < 2; ++s) {
            Fnv f;
            for (std::size_t i = 0; i < blocks.size(); i += 16) {
                std::uint16_t out[16][3];
                ma::decodeBc6hBlock(&blocks[i], s != 0, out);
                f.bytes(out, 96);
            }
            CHECK_MSG(f.h == (s ? kBc6SignedDigest : kBc6UnsignedDigest), "BC6H %s digest %016llx", s ? "signed" : "unsigned",
                      static_cast<unsigned long long>(f.h));
        }
    }
    {
        const Bytes blocks = bc4Blocks();
        for (int s = 0; s < 2; ++s) {
            Fnv f;
            for (std::size_t i = 0; i < blocks.size(); i += 8) {
                float out[16];
                ma::decodeBc4Block(&blocks[i], s != 0, out);
                for (float v : out) {
                    f.u32(static_cast<std::uint32_t>(quantise(v)));
                }
            }
            CHECK_MSG(f.h == (s ? kBc4SnormDigest : kBc4UnormDigest), "BC4 %s digest", s ? "snorm" : "unorm");
        }
        // BC5 = two BC4 halves: green of BC5 equals the BC4 decode of the second half.
        auto bc5img = ma::decodeToRgba32f(TexFormat::BC5_UNORM_BLOCK, 4, 4, 1, std::span(blocks).first(16));
        float g[16];
        ma::decodeBc4Block(&blocks[8], false, g);
        CHECK(bc5img && (*bc5img)[1] == g[0] && (*bc5img)[15 * 4 + 1] == g[15]);
    }
    // Partial blocks: a 5x3 BC1 image uses 2x1 blocks and drops the texels outside the extent.
    Bytes two = bc1;
    two.insert(two.end(), bc1t.begin(), bc1t.end());
    px = ma::decodeToRgba32f(TexFormat::BC1_RGBA_UNORM_BLOCK, 5, 3, 1, two);
    CHECK(px && px->size() == 5 * 3 * 4 && texelIs(*px, 3, twoThirds) && texelIs(*px, 4, blue) && texelIs(*px, 5 + 4, blue));
}

// ---- DDS -------------------------------------------------------------------------------------------------

std::vector<TexFormat> allFormats() {
    std::vector<TexFormat> out;
    for (std::uint32_t v = 0; v < 200; ++v) {
        if (ma::texFormatInfo(static_cast<TexFormat>(v))) {
            out.push_back(static_cast<TexFormat>(v));
        }
    }
    return out;
}

ma::TextureImage makeImage(TexFormat f, ma::TexDimension dim, std::uint32_t w, std::uint32_t h, std::uint32_t d,
                           std::uint32_t mips, std::uint32_t layers, std::uint64_t seed) {
    ma::TextureImage img;
    img.format = f;
    img.dimension = dim;
    img.width = w;
    img.height = h;
    img.depth = d;
    img.mipLevels = mips;
    img.arraySize = layers;
    img.faces = dim == ma::TexDimension::Cube ? 6 : 1;
    const std::uint64_t size = ma::layoutSubresources(img);
    img.data = randomBytes(static_cast<std::size_t>(size), seed);
    return img;
}

bool sameImage(const ma::TextureImage& a, const ma::TextureImage& b) {
    return a.format == b.format && a.dimension == b.dimension && a.width == b.width && a.height == b.height && a.depth == b.depth &&
           a.mipLevels == b.mipLevels && a.arraySize == b.arraySize && a.faces == b.faces && a.data == b.data &&
           a.subresources.size() == b.subresources.size();
}

void testDdsDx10() {
    // Vulkan formats DXGI cannot spell (only reachable from DX9 headers or packages).
    const TexFormat noDxgi[] = {TexFormat::A2R10G10B10_UNORM_PACK32, TexFormat::BC1_RGB_UNORM_BLOCK, TexFormat::BC1_RGB_SRGB_BLOCK};
    int roundTrips = 0, expected = 0;
    for (TexFormat f : allFormats()) {
        const bool spelled = std::find(std::begin(noDxgi), std::end(noDxgi), f) == std::end(noDxgi);
        CHECK_MSG((ma::dxgiFromTexFormat(f) != 0) == spelled, "%s DXGI spelling", ma::texFormatInfo(f)->name);
        if (!spelled) {
            continue;
        }
        expected += 5;
        const std::uint64_t seed = static_cast<std::uint64_t>(f) * 7919u;
        const ma::TextureImage shapes[] = {
            makeImage(f, ma::TexDimension::Tex2D, 13, 7, 1, 4, 1, seed),
            makeImage(f, ma::TexDimension::Tex2D, 8, 8, 1, 1, 3, seed + 1),
            makeImage(f, ma::TexDimension::Cube, 8, 8, 1, 4, 2, seed + 2),
            makeImage(f, ma::TexDimension::Tex3D, 6, 5, 4, 3, 1, seed + 3),
            makeImage(f, ma::TexDimension::Tex1D, 9, 1, 1, 2, 2, seed + 4),
        };
        for (const ma::TextureImage& img : shapes) {
            std::string err;
            const Bytes file = ma::writeDds(img, &err);
            CHECK_MSG(!file.empty(), "%s: %s", ma::texFormatInfo(f)->name, err.c_str());
            auto back = ma::readDds(file, &err);
            CHECK_MSG(back && sameImage(back->image, img) && back->info.dx10Header && back->info.trailingBytes == 0,
                      "%s dim %d: %s", ma::texFormatInfo(f)->name, int(img.dimension), err.c_str());
            if (back && back->image.format == f) {
                ++roundTrips;
                // Every subresource decodes.
                for (const ma::Subresource& s : back->image.subresources) {
                    if (!ma::decodeSubresource(back->image, s)) {
                        CHECK_MSG(false, "%s subresource decode", ma::texFormatInfo(f)->name);
                        break;
                    }
                }
            }
        }
    }
    CHECK(roundTrips == expected && expected > 300);

    // DXGI mapping details: TYPELESS -> UNORM, X formats swizzle alpha to one, A8 to 000r, alpha mode.
    ma::Swizzle sw;
    CHECK(ma::texFormatFromDxgi(94, &sw) == TexFormat::BC6H_UFLOAT_BLOCK && sw.identity());
    CHECK(ma::texFormatFromDxgi(88, &sw) == TexFormat::B8G8R8A8_UNORM && sw.a == Swz::One);
    CHECK(ma::texFormatFromDxgi(65, &sw) == TexFormat::R8_UNORM && sw.r == Swz::Zero && sw.a == Swz::R);
    CHECK(ma::texFormatFromDxgi(1234) == TexFormat::Undefined);
    CHECK(ma::dxgiFromTexFormat(TexFormat::BC7_SRGB_BLOCK) == 99 && ma::dxgiFromTexFormat(TexFormat::BC7_UNORM_BLOCK) == 98 &&
          ma::dxgiFromTexFormat(TexFormat::R8G8B8A8_UNORM) == 28 && ma::dxgiFromTexFormat(TexFormat::B8G8R8A8_UNORM) == 87);
    ma::TextureImage pm = makeImage(TexFormat::BC3_UNORM_BLOCK, ma::TexDimension::Tex2D, 4, 4, 1, 1, 1, 5);
    pm.premultipliedAlpha = true;
    auto pmBack = ma::readDds(ma::writeDds(pm));
    CHECK(pmBack && pmBack->image.premultipliedAlpha);

    // Trailing bytes are tolerated and reported; truncation is not.
    const ma::TextureImage base = makeImage(TexFormat::BC1_RGBA_UNORM_BLOCK, ma::TexDimension::Tex2D, 16, 16, 1, 5, 1, 9);
    Bytes file = ma::writeDds(base);
    file.push_back(0xaa);
    auto t = ma::readDds(file);
    CHECK(t && t->info.trailingBytes == 1 && t->image.data == base.data);
    file.resize(file.size() - 2);
    std::string err;
    CHECK(!ma::readDds(file, &err) && err.find("truncated") != std::string::npos);
    // Subresource lookup follows the file order.
    CHECK(t && t->image.subresource(0, 0, 4)->width == 1 && t->image.subresource(0, 0, 5) == nullptr &&
          t->image.subresource(0, 0, 1)->offset == 128);
}

/// A DX9-header DDS file.
Bytes legacyDds(std::uint32_t pfFlags, std::uint32_t fourCC, std::uint32_t bits, std::uint32_t r, std::uint32_t g, std::uint32_t b,
                std::uint32_t a, std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t caps2, std::uint32_t depth,
                const Bytes& payload) {
    Bytes o;
    put32(o, 0x20534444);
    put32(o, 124);
    put32(o, 0x1007 | (mips > 1 ? 0x20000u : 0u) | (depth ? 0x800000u : 0u));
    put32(o, h);
    put32(o, w);
    put32(o, 0);
    put32(o, depth);
    put32(o, mips);
    for (int k = 0; k < 11; ++k) {
        put32(o, 0);
    }
    put32(o, 32);
    put32(o, pfFlags);
    put32(o, fourCC);
    put32(o, bits);
    put32(o, r);
    put32(o, g);
    put32(o, b);
    put32(o, a);
    put32(o, 0x1000);
    put32(o, caps2);
    put32(o, 0);
    put32(o, 0);
    put32(o, 0);
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

constexpr std::uint32_t kFourCC = 0x4, kRgb = 0x40, kAlphaPx = 0x1, kAlpha = 0x2, kLum = 0x20000, kBump = 0x80000;

std::uint32_t cc(const char* s) {
    return std::uint32_t(std::uint8_t(s[0])) | (std::uint32_t(std::uint8_t(s[1])) << 8) | (std::uint32_t(std::uint8_t(s[2])) << 16) |
           (std::uint32_t(std::uint8_t(s[3])) << 24);
}

struct LegacyCase {
    const char* name;
    std::uint32_t flags, fourCC, bits, r, g, b, a;
    Bytes texel; // one texel (or one block)
    TexFormat format;
    ma::Swizzle swizzle;
    bool converted;
    float expect[4]; // decoded texel 0 after the swizzle
};

void testDdsLegacy() {
    const ma::Swizzle id{}, opaque{Swz::R, Swz::G, Swz::B, Swz::One}, lum{Swz::R, Swz::R, Swz::R, Swz::One},
        lumA{Swz::R, Swz::R, Swz::R, Swz::G}, alpha{Swz::Zero, Swz::Zero, Swz::Zero, Swz::R};
    const Bytes dxt1 = {0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0};
    Bytes dxt5 = {255, 0, 0, 0, 0, 0, 0, 0};
    dxt5.insert(dxt5.end(), dxt1.begin(), dxt1.end());
    const Bytes ati = {255, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0};
    const std::vector<LegacyCase> cases = {
        {"A8R8G8B8", kRgb | kAlphaPx, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, {51, 102, 255, 0}, TexFormat::B8G8R8A8_UNORM, id, false, {1, 0.4f, 0.2f, 0}},
        {"X8R8G8B8", kRgb, 0, 32, 0xff0000, 0xff00, 0xff, 0, {51, 102, 255, 0}, TexFormat::B8G8R8A8_UNORM, opaque, false, {1, 0.4f, 0.2f, 1}},
        {"A8B8G8R8", kRgb | kAlphaPx, 0, 32, 0xff, 0xff00, 0xff0000, 0xff000000, {51, 102, 255, 0}, TexFormat::R8G8B8A8_UNORM, id, false, {0.2f, 0.4f, 1, 0}},
        {"X8B8G8R8", kRgb, 0, 32, 0xff, 0xff00, 0xff0000, 0, {51, 102, 255, 0}, TexFormat::R8G8B8A8_UNORM, opaque, false, {0.2f, 0.4f, 1, 1}},
        {"G16R16", kRgb, 0, 32, 0xffff, 0xffff0000, 0, 0, b16({0xffff, 0}), TexFormat::R16G16_UNORM, id, false, {1, 0, 0, 1}},
        {"A2B10G10R10", kRgb | kAlphaPx, 0, 32, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000, b32({0xc00003ffu}), TexFormat::A2B10G10R10_UNORM_PACK32, id, false, {1, 0, 0, 1}},
        {"A2R10G10B10", kRgb | kAlphaPx, 0, 32, 0x3ff00000, 0xffc00, 0x3ff, 0xc0000000, b32({0x000003ffu}), TexFormat::A2R10G10B10_UNORM_PACK32, id, false, {0, 0, 1, 0}},
        {"R32F by mask", kRgb, 0, 32, 0xffffffff, 0, 0, 0, bf({2.5f}), TexFormat::R32_SFLOAT, id, false, {2.5f, 0, 0, 1}},
        {"R8G8B8", kRgb, 0, 24, 0xff0000, 0xff00, 0xff, 0, {51, 102, 255}, TexFormat::R8G8B8A8_UNORM, id, true, {1, 0.4f, 0.2f, 1}},
        {"R5G6B5", kRgb, 0, 16, 0xf800, 0x7e0, 0x1f, 0, b16({0xf800}), TexFormat::R5G6B5_UNORM_PACK16, id, false, {1, 0, 0, 1}},
        {"A1R5G5B5", kRgb | kAlphaPx, 0, 16, 0x7c00, 0x3e0, 0x1f, 0x8000, b16({0x801f}), TexFormat::A1R5G5B5_UNORM_PACK16, id, false, {0, 0, 1, 1}},
        {"X1R5G5B5", kRgb, 0, 16, 0x7c00, 0x3e0, 0x1f, 0, b16({0x001f}), TexFormat::A1R5G5B5_UNORM_PACK16, opaque, false, {0, 0, 1, 1}},
        {"A4R4G4B4", kRgb | kAlphaPx, 0, 16, 0xf00, 0xf0, 0xf, 0xf000, b16({0x5f0a}), TexFormat::R8G8B8A8_UNORM, id, true, {1, 0, 170 / 255.f, 85 / 255.f}},
        {"X4R4G4B4", kRgb, 0, 16, 0xf00, 0xf0, 0xf, 0, b16({0x5f0a}), TexFormat::R8G8B8A8_UNORM, id, true, {1, 0, 170 / 255.f, 1}},
        {"A8R3G3B2", kRgb | kAlphaPx, 0, 16, 0xe0, 0x1c, 0x3, 0xff00, {0xe3, 0x80}, TexFormat::R8G8B8A8_UNORM, id, true, {1, 0, 1, 128 / 255.f}},
        {"R3G3B2", kRgb, 0, 8, 0xe0, 0x1c, 0x3, 0, {0x1c}, TexFormat::R8G8B8A8_UNORM, id, true, {0, 1, 0, 1}},
        {"L8", kLum, 0, 8, 0xff, 0, 0, 0, {51}, TexFormat::R8_UNORM, lum, false, {0.2f, 0.2f, 0.2f, 1}},
        {"L8 as RGB", kRgb, 0, 8, 0xff, 0, 0, 0, {51}, TexFormat::R8_UNORM, lum, false, {0.2f, 0.2f, 0.2f, 1}},
        {"L16", kLum, 0, 16, 0xffff, 0, 0, 0, b16({0xffff}), TexFormat::R16_UNORM, lum, false, {1, 1, 1, 1}},
        {"A8L8", kLum | kAlphaPx, 0, 16, 0xff, 0, 0, 0xff00, {51, 102}, TexFormat::R8G8_UNORM, lumA, false, {0.2f, 0.2f, 0.2f, 0.4f}},
        {"A4L4", kLum | kAlphaPx, 0, 8, 0xf, 0, 0, 0xf0, {0x5a}, TexFormat::R8G8B8A8_UNORM, id, true, {170 / 255.f, 170 / 255.f, 170 / 255.f, 85 / 255.f}},
        {"A8", kAlpha, 0, 8, 0, 0, 0, 0xff, {102}, TexFormat::R8_UNORM, alpha, false, {0, 0, 0, 0.4f}},
        {"V8U8", kBump, 0, 16, 0xff, 0xff00, 0, 0, {0x7f, 0x81}, TexFormat::R8G8_SNORM, id, false, {1, -1, 0, 1}},
        {"Q8W8V8U8", kBump, 0, 32, 0xff, 0xff00, 0xff0000, 0xff000000, {0x7f, 0, 0x81, 0x7f}, TexFormat::R8G8B8A8_SNORM, id, false, {1, 0, -1, 1}},
        {"V16U16", kBump, 0, 32, 0xffff, 0xffff0000, 0, 0, b16({0x7fff, 0x8001}), TexFormat::R16G16_SNORM, id, false, {1, -1, 0, 1}},
        {"A16B16G16R16", kFourCC, 36, 0, 0, 0, 0, 0, b16({0xffff, 0, 0xffff, 0}), TexFormat::R16G16B16A16_UNORM, id, false, {1, 0, 1, 0}},
        {"Q16W16V16U16", kFourCC, 110, 0, 0, 0, 0, 0, b16({0x7fff, 0, 0x8001, 0}), TexFormat::R16G16B16A16_SNORM, id, false, {1, 0, -1, 0}},
        {"R16F", kFourCC, 111, 0, 0, 0, 0, 0, b16({f2h(0.5f)}), TexFormat::R16_SFLOAT, id, false, {0.5f, 0, 0, 1}},
        {"G16R16F", kFourCC, 112, 0, 0, 0, 0, 0, b16({f2h(0.5f), f2h(2.0f)}), TexFormat::R16G16_SFLOAT, id, false, {0.5f, 2, 0, 1}},
        {"A16B16G16R16F", kFourCC, 113, 0, 0, 0, 0, 0, b16({f2h(0.5f), f2h(2.0f), 0, 0x3c00}), TexFormat::R16G16B16A16_SFLOAT, id, false, {0.5f, 2, 0, 1}},
        {"R32F", kFourCC, 114, 0, 0, 0, 0, 0, bf({-3.0f}), TexFormat::R32_SFLOAT, id, false, {-3, 0, 0, 1}},
        {"G32R32F", kFourCC, 115, 0, 0, 0, 0, 0, bf({1, 2}), TexFormat::R32G32_SFLOAT, id, false, {1, 2, 0, 1}},
        {"A32B32G32R32F", kFourCC, 116, 0, 0, 0, 0, 0, bf({1, 2, 3, 4}), TexFormat::R32G32B32A32_SFLOAT, id, false, {1, 2, 3, 4}},
        {"DXT1", kFourCC, cc("DXT1"), 0, 0, 0, 0, 0, dxt1, TexFormat::BC1_RGBA_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"DXT3", kFourCC, cc("DXT3"), 0, 0, 0, 0, 0, dxt5, TexFormat::BC2_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"DXT5", kFourCC, cc("DXT5"), 0, 0, 0, 0, 0, dxt5, TexFormat::BC3_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"ATI1", kFourCC, cc("ATI1"), 0, 0, 0, 0, 0, Bytes(ati.begin(), ati.begin() + 8), TexFormat::BC4_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"BC4U", kFourCC, cc("BC4U"), 0, 0, 0, 0, 0, Bytes(ati.begin(), ati.begin() + 8), TexFormat::BC4_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"ATI2", kFourCC, cc("ATI2"), 0, 0, 0, 0, 0, ati, TexFormat::BC5_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
        {"BC5U", kFourCC, cc("BC5U"), 0, 0, 0, 0, 0, ati, TexFormat::BC5_UNORM_BLOCK, id, false, {1, 0, 0, 1}},
    };
    for (const LegacyCase& c : cases) {
        const Bytes file = legacyDds(c.flags, c.fourCC, c.bits, c.r, c.g, c.b, c.a, 1, 1, 1, 0, 0, c.texel);
        std::string err;
        auto t = ma::readDds(file, &err);
        CHECK_MSG(t.has_value(), "%s: %s", c.name, err.c_str());
        if (!t) {
            continue;
        }
        CHECK_MSG(t->image.format == c.format && t->image.swizzle == c.swizzle && t->info.converted == c.converted &&
                      !t->info.dx10Header && t->info.trailingBytes == 0,
                  "%s mapping", c.name);
        auto px = ma::decodeSubresource(t->image, t->image.subresources[0]);
        CHECK_MSG(px.has_value(), "%s decode", c.name);
        if (px) {
            ma::applySwizzle(*px, t->image.swizzle);
            CHECK_MSG(texelIs(*px, 0, c.expect, 1e-5f), "%s texel", c.name);
        }
    }
    // DXT2 / DXT4 are premultiplied BC2 / BC3.
    auto dxt2 = ma::readDds(legacyDds(kFourCC, cc("DXT2"), 0, 0, 0, 0, 0, 4, 4, 1, 0, 0, dxt5));
    auto dxt4 = ma::readDds(legacyDds(kFourCC, cc("DXT4"), 0, 0, 0, 0, 0, 4, 4, 1, 0, 0, dxt5));
    CHECK(dxt2 && dxt2->image.format == TexFormat::BC2_UNORM_BLOCK && dxt2->image.premultipliedAlpha);
    CHECK(dxt4 && dxt4->image.format == TexFormat::BC3_UNORM_BLOCK && dxt4->image.premultipliedAlpha);
    // Unsupported legacy formats fail cleanly.
    std::string err;
    CHECK(!ma::readDds(legacyDds(kFourCC, cc("UYVY"), 0, 0, 0, 0, 0, 2, 1, 1, 0, 0, Bytes(4)), &err) &&
          err.find("unsupported") != std::string::npos);
    CHECK(!ma::readDds(legacyDds(kRgb, 0, 16, 0x1f, 0x3e0, 0xfc00, 0, 1, 1, 1, 0, 0, Bytes(2))));

    // Cube (all six faces, mips) and volume.
    const std::uint32_t cubeAll = 0x200 | 0xfc00;
    const Bytes cubeData = randomBytes(6 * (64 + 16 + 4) * 4, 77);
    auto cube = ma::readDds(legacyDds(kRgb | kAlphaPx, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 8, 8, 3, cubeAll, 0, cubeData));
    CHECK(cube && cube->image.dimension == ma::TexDimension::Cube && cube->image.faces == 6 && cube->image.subresources.size() == 18 &&
          cube->image.subresource(0, 5, 2)->offset == 5 * 84 * 4 + 80 * 4 && cube->image.data == cubeData);
    CHECK(!ma::readDds(legacyDds(kRgb | kAlphaPx, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 8, 8, 1, 0x200 | 0x400, 0, cubeData), &err) &&
          err.find("partial") != std::string::npos);
    const Bytes volData = randomBytes((4 * 4 * 4 + 2 * 2 * 2 + 1) * 2, 78);
    auto vol = ma::readDds(legacyDds(kLum | kAlphaPx, 0, 16, 0xff, 0, 0, 0xff00, 4, 4, 3, 0x200000, 4, volData));
    CHECK(vol && vol->image.dimension == ma::TexDimension::Tex3D && vol->image.depth == 4 && vol->image.subresource(0, 0, 2)->depth == 1 &&
          vol->image.data == volData);
}

void testDdsMalformed() {
    const ma::TextureImage img = makeImage(TexFormat::BC7_UNORM_BLOCK, ma::TexDimension::Tex2D, 16, 8, 1, 2, 1, 3);
    const Bytes good = ma::writeDds(img);
    auto patch = [&](std::size_t at, std::uint32_t v) {
        Bytes b = good;
        for (int k = 0; k < 4; ++k) {
            b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
        }
        return b;
    };
    std::string err;
    CHECK(ma::readDds(good).has_value());
    CHECK(!ma::readDds(Bytes(good.begin(), good.begin() + 100), &err));
    CHECK(!ma::readDds(patch(0, 0x12345678), &err) && err == "not a DDS file");
    CHECK(!ma::readDds(patch(4, 100), &err));                    // header size
    CHECK(!ma::readDds(patch(4 + 72, 31), &err));                // pixel format size
    CHECK(!ma::readDds(patch(4 + 24, 5), &err));                 // 5 mips for 16x8
    CHECK(!ma::readDds(patch(4 + 12, 0), &err));                 // width 0
    CHECK(!ma::readDds(patch(4 + 12, 0x10000000), &err));        // absurd width
    CHECK(!ma::readDds(patch(128, 1234), &err) && err.find("DXGI") != std::string::npos);
    CHECK(!ma::readDds(patch(132, 7), &err));                    // resource dimension
    CHECK(!ma::readDds(patch(140, 0), &err));                    // array size 0
    CHECK(!ma::readDds(patch(140, 1000), &err) && err.find("truncated") != std::string::npos);
    CHECK(!ma::readDds(Bytes(good.begin(), good.begin() + 140), &err));
    CHECK(!ma::readDdsFile("/nonexistent/relight/none.dds", &err));
}

#if FUSE_RL_MODS_HAVE_CAPTURE_EXPORT
// RL-1.8's writer produces DX9 files in Remix's canonical layout: this reader must read every one of them.
void testDdsCaptureWriter() {
    namespace ex = fuse::relight::capture::exporter;
    using D = rh::D3DFormat;
    const D formats[] = {D::R8G8B8,   D::A8R8G8B8, D::X8R8G8B8, D::R5G6B5,       D::X1R5G5B5,      D::A1R5G5B5, D::A4R4G4B4,
                         D::R3G3B2,   D::A8,       D::A8R3G3B2, D::X4R4G4B4,     D::A2B10G10R10,   D::A8B8G8R8, D::X8B8G8R8,
                         D::G16R16,   D::A2R10G10B10, D::A16B16G16R16, D::L8,    D::A8L8,          D::A4L4,     D::V8U8,
                         D::L6V5U5,   D::X8L8V8U8, D::Q8W8V8U8, D::V16U16,       D::A2W10V10U10,   D::L16,      D::Q16W16V16U16,
                         D::R16F,     D::G16R16F,  D::A16B16G16R16F, D::R32F,    D::G32R32F,       D::A32B32G32R32F,
                         D::CxV8U8,   D::DXT1,     D::DXT2,     D::DXT3,         D::DXT4,          D::DXT5,     D::ATI1,
                         D::ATI2,     D::UYVY,     D::YUY2};
    const D unsupported[] = {D::L6V5U5, D::X8L8V8U8, D::A2W10V10U10, D::CxV8U8, D::UYVY, D::YUY2};
    int read = 0, rejected = 0, decoded = 0;
    for (D f : formats) {
        ex::DdsImage src;
        src.format = f;
        src.width = 9;
        src.height = 6;
        for (std::uint32_t m = 0; m < 3; ++m) {
            const auto l = rh::textureMip0Layout(f, std::max(1u, 9u >> m), std::max(1u, 6u >> m), 1);
            src.mips.push_back(randomBytes(static_cast<std::size_t>(l.size), static_cast<std::uint64_t>(f) + m));
        }
        std::string err;
        const Bytes file = ex::writeDds(src, &err);
        if (file.empty()) {
            continue; // not writable by RL-1.8 (no single-plane layout)
        }
        auto t = ma::readDds(file, &err);
        const bool expectUnsupported = std::find(std::begin(unsupported), std::end(unsupported), f) != std::end(unsupported);
        if (!t) {
            CHECK_MSG(expectUnsupported, "D3DFORMAT %u: %s", static_cast<unsigned>(f), err.c_str());
            rejected += expectUnsupported ? 1 : 0;
            continue;
        }
        ++read;
        CHECK_MSG(!expectUnsupported, "D3DFORMAT %u unexpectedly read", static_cast<unsigned>(f));
        CHECK_MSG(t->info.legacyFormat == f && t->image.mipLevels == 3 && t->image.width == 9 && t->image.height == 6 &&
                      t->info.trailingBytes == 0,
                  "D3DFORMAT %u header", static_cast<unsigned>(f));
        if (!t->info.converted) {
            const Bytes payload(file.begin() + 128, file.end());
            CHECK_MSG(t->image.data == payload, "D3DFORMAT %u payload", static_cast<unsigned>(f));
        }
        // Same RGBA8 as RL-1.8's canonical decode, wherever that one is defined.
        const auto theirs = ex::decodeRgba8(f, 9, 6, src.mips[0]);
        if (!theirs) {
            continue;
        }
        auto mine = ma::decodeSubresource(t->image, t->image.subresources[0]);
        CHECK(mine.has_value());
        if (!mine) {
            continue;
        }
        ma::applySwizzle(*mine, t->image.swizzle);
        std::size_t diff = 0;
        for (std::size_t i = 0; i < theirs->size(); ++i) {
            const long v = std::lround((*mine)[i] * 255.0f);
            diff += v == (*theirs)[i] ? 0 : 1;
        }
        CHECK_MSG(diff == 0, "D3DFORMAT %u: %zu of %zu channels differ from capture/export decodeRgba8", static_cast<unsigned>(f), diff,
                  theirs->size());
        ++decoded;
    }
    CHECK_MSG(read >= 36 && rejected == 6 && decoded >= 20, "read %d rejected %d decoded %d", read, rejected, decoded);

    // BC1-BC3 palettes agree with capture/export over many random blocks.
    for (D f : {D::DXT1, D::DXT3, D::DXT5}) {
        const std::uint32_t w = 64, h = 64;
        const auto l = rh::textureMip0Layout(f, w, h, 1);
        const Bytes blocks = randomBytes(static_cast<std::size_t>(l.size), 1000 + static_cast<std::uint64_t>(f));
        const auto theirs = ex::decodeRgba8(f, w, h, blocks);
        const TexFormat mf = f == D::DXT1 ? TexFormat::BC1_RGBA_UNORM_BLOCK : f == D::DXT3 ? TexFormat::BC2_UNORM_BLOCK : TexFormat::BC3_UNORM_BLOCK;
        const auto mine = ma::decodeToRgba32f(mf, w, h, 1, blocks);
        bool same = theirs && mine && theirs->size() == mine->size();
        for (std::size_t i = 0; same && i < theirs->size(); ++i) {
            same = std::lround((*mine)[i] * 255.0f) == (*theirs)[i];
        }
        CHECK_MSG(same, "BC palette of D3DFORMAT %u differs from capture/export", static_cast<unsigned>(f));
    }
}
#endif

// ---- GDeflate ------------------------------------------------------------------------------------------------

void testGdeflate() {
    CHECK(gd::available());
    CHECK(gd::crc32(Bytes{'1', '2', '3', '4', '5', '6', '7', '8', '9'}) == 0xcbf43926u); // the CRC-32 check value
    CHECK(gd::crc32(Bytes{}) == 0);

    std::vector<std::pair<std::string, Bytes>> inputs;
    inputs.emplace_back("1 byte", Bytes{42});
    inputs.emplace_back("zeros 64K", Bytes(65536, 0));
    inputs.emplace_back("zeros 64K+1", Bytes(65537, 0));
    inputs.emplace_back("random 200000", randomBytes(200000, 11));
    {
        std::string text;
        for (int i = 0; text.size() < 300000; ++i) {
            text += "prim mesh_" + std::to_string(i * 7919 % 1000) + " { material = mat_" + std::to_string(i % 37) + "; }\n";
        }
        inputs.emplace_back("text 300K", Bytes(text.begin(), text.end()));
    }
    {
        Bytes bc = rl_bc_oracle::bc7Blocks(); // texture-like data
        for (int k = 0; k < 4; ++k) {
            bc.insert(bc.end(), bc.begin(), bc.begin() + 30000);
        }
        inputs.emplace_back("bc7 blocks", bc);
    }
    for (const auto& [name, data] : inputs) {
        for (std::uint32_t level : {1u, 6u, 9u, 12u}) {
            std::string err;
            auto stream = gd::compress(data, level, &err);
            CHECK_MSG(stream.has_value(), "%s L%u: %s", name.c_str(), level, err.c_str());
            if (!stream) {
                continue;
            }
            auto info = gd::inspect(*stream, &err);
            CHECK_MSG(info && info->uncompressedSize == data.size() && info->streamSize == stream->size() &&
                          info->numTiles == (data.size() + gd::kTileSize - 1) / gd::kTileSize,
                      "%s L%u inspect: %s", name.c_str(), level, err.c_str());
            auto back = gd::decompress(*stream, &err);
            CHECK_MSG(back && *back == data, "%s L%u round trip: %s", name.c_str(), level, err.c_str());
            Bytes ref(data.size());
            CHECK_MSG(gd::referenceDecompress(*stream, ref) && ref == data, "%s L%u reference decoder", name.c_str(), level);
            // The reference compressor is deterministic.
            auto again = gd::compress(data, level);
            CHECK(again && *again == *stream);
        }
    }
    // Compression actually compresses redundant data.
    auto z = gd::compress(Bytes(65536 * 3, 7), 9);
    CHECK(z && z->size() < 4096);

    // Errors: wrong output size, bad header, truncated, corrupt tile data, the size cap.
    const Bytes data = randomBytes(150000, 12);
    const Bytes good = *gd::compress(data, 6);
    Bytes out(data.size() - 1);
    std::string err;
    CHECK(gd::decompress(good, out, &err) == gd::Status::OutputSize);
    Bytes bad = good;
    bad[1] ^= 1;
    CHECK(gd::decompress(bad, &err) == std::nullopt && !gd::inspect(bad));
    bad = good;
    bad[4] = static_cast<std::uint8_t>((bad[4] & ~3u) | 2u); // tileSizeIdx 2
    CHECK(!gd::inspect(bad));
    CHECK(!gd::inspect(Bytes(good.begin(), good.end() - 1)));
    CHECK(!gd::inspect(Bytes(good.begin(), good.begin() + 10)));
    CHECK(!gd::decompress(good, &err, 1000) && err.find("allowed") != std::string::npos);
    int caught = 0;
    for (std::size_t at = 20; at < good.size(); at += good.size() / 23) {
        bad = good;
        bad[at] ^= 0x5a;
        Bytes o(data.size());
        const gd::Status s = gd::decompress(bad, o, &err);
        caught += (s != gd::Status::Ok || o != data) ? 1 : 0;
    }
    CHECK_MSG(caught >= 20, "corrupted streams detected or decoded differently: %d", caught);
    CHECK(std::string(gd::statusName(gd::Status::CodecUnavailable)).find("FUSE_RELIGHT_GDEFLATE") != std::string::npos);
}

// ---- packages ------------------------------------------------------------------------------------------------

void testPackageLayout() {
    // Hand-built package with the exact upstream struct layout: header, one 2D asset, one raw blob.
    Bytes pkg;
    put32(pkg, 0xbaadd00d);
    put32(pkg, 1);
    put32(pkg, 16 + 16); // dictOffset (low)
    put32(pkg, 0);
    const Bytes texels = randomBytes(16, 5); // 2x2 R8G8B8A8
    pkg.insert(pkg.end(), texels.begin(), texels.end());
    put16(pkg, 1); // assets
    put16(pkg, 1); // blobs
    // AssetDesc (20 bytes)
    put16(pkg, 0);
    pkg.push_back(2);  // IMAGE_2D
    pkg.push_back(37); // VK_FORMAT_R8G8B8A8_UNORM
    put16(pkg, 2);
    put16(pkg, 2);
    put16(pkg, 1);  // depth
    put16(pkg, 1);  // numMips
    put16(pkg, 0);  // numTailMips
    put16(pkg, 1);  // arraySize
    put16(pkg, 0);  // baseBlobIdx
    put16(pkg, 0);  // tailBlobIdx
    // BlobDesc (16 bytes): offset:40 = 16, compression:8 = 0, flags:8 = 0x5a
    put32(pkg, 16);
    put32(pkg, 0x5a0000);
    put32(pkg, 16);
    put32(pkg, gd::crc32(texels));
    const std::string name = "textures/a.dds";
    pkg.insert(pkg.end(), name.begin(), name.end());
    pkg.push_back(0);

    std::string err;
    auto p = ma::AssetPackage::fromBytes(pkg, &err);
    CHECK_MSG(p.has_value(), "%s", err.c_str());
    if (!p) {
        return;
    }
    CHECK(p->assetCount() == 1 && p->blobCount() == 1 && p->dataSize() == 32);
    CHECK(p->blob(0)->offset == 16 && p->blob(0)->flags == 0x5a && p->blob(0)->compression == 0 && p->blob(0)->size == 16);
    CHECK(p->asset(0)->type == ma::PackageAssetType::Image2D && p->asset(0)->format == 37 && p->asset(0)->width == 2 &&
          p->asset(0)->size == 0x00020002u);
    CHECK(p->findAsset(name) == 0 && p->findAsset("missing") == ma::AssetPackage::kNoIndex && p->assetName(0) == name);
    CHECK(p->blobCrcMatches(0));
    auto img = p->loadImage(0, &err);
    CHECK(img && img->format == TexFormat::R8G8B8A8_UNORM && img->data == texels);

    // Malformed variants.
    auto patch = [&](std::size_t at, std::uint8_t v) {
        Bytes b = pkg;
        b[at] = v;
        return b;
    };
    CHECK(!ma::AssetPackage::fromBytes(patch(0, 0), &err) && err == "not an asset package");
    CHECK(!ma::AssetPackage::fromBytes(patch(4, 2), &err) && err.find("version") != std::string::npos);
    CHECK(!ma::AssetPackage::fromBytes(patch(8, 200), &err));       // dictionary past EOF
    CHECK(!ma::AssetPackage::fromBytes(patch(32, 9), &err));        // 9 assets: truncated dictionary
    CHECK(!ma::AssetPackage::fromBytes(Bytes(pkg.begin(), pkg.end() - 1), &err) && err.find("name") != std::string::npos);
    CHECK(!ma::AssetPackage::fromBytes(Bytes(pkg.begin(), pkg.begin() + 10), &err));
    auto shifted = ma::AssetPackage::fromBytes(patch(56, 200)); // blob offset past EOF
    CHECK(shifted && !shifted->readBlobRaw(0, &err) && !shifted->loadImage(0));
    auto wrongFormat = ma::AssetPackage::fromBytes(patch(39, 250));
    CHECK(wrongFormat && !wrongFormat->loadImage(0, &err) && err.find("VkFormat") != std::string::npos);
    CHECK(!p->loadBuffer(0) && !p->loadImage(7) && p->blobIndex(0, 1, 0, 0) == ma::AssetPackage::kNoIndex);
}

void testPackageRoundTrip() {
    struct Case {
        const char* name;
        ma::TextureImage image;
        std::uint32_t tailMips;
    };
    std::vector<Case> cases;
    cases.push_back({"bc7 2d tail", makeImage(TexFormat::BC7_SRGB_BLOCK, ma::TexDimension::Tex2D, 64, 32, 1, 7, 1, 1), 3});
    cases.push_back({"bc1 2d no tail", makeImage(TexFormat::BC1_RGBA_UNORM_BLOCK, ma::TexDimension::Tex2D, 20, 12, 1, 5, 1, 2), 0});
    cases.push_back({"bc6h cube array tail", makeImage(TexFormat::BC6H_UFLOAT_BLOCK, ma::TexDimension::Cube, 16, 16, 1, 5, 2, 3), 2});
    cases.push_back({"rgba8 array tail gaps", makeImage(TexFormat::R8G8B8A8_UNORM, ma::TexDimension::Tex2D, 16, 16, 1, 5, 3, 4), 2});
    cases.push_back({"rgba16f volume", makeImage(TexFormat::R16G16B16A16_SFLOAT, ma::TexDimension::Tex3D, 8, 8, 4, 4, 1, 5), 1});
    cases.push_back({"r32f 1d", makeImage(TexFormat::R32_SFLOAT, ma::TexDimension::Tex1D, 33, 1, 1, 2, 1, 6), 0});
    cases.push_back({"bc5 all tail", makeImage(TexFormat::BC5_UNORM_BLOCK, ma::TexDimension::Tex2D, 8, 8, 1, 4, 1, 7), 4});
    for (std::uint32_t level : {0u, 9u}) {
        ma::AssetPackageWriter w;
        std::string err;
        for (const Case& c : cases) {
            CHECK_MSG(w.addImage(c.name, c.image, c.tailMips, level, &err), "%s: %s", c.name, err.c_str());
        }
        ma::PackageAssetDesc buf;
        buf.type = ma::PackageAssetType::Buffer;
        const Bytes bufData = randomBytes(70000, 8);
        buf.size = static_cast<std::uint32_t>(bufData.size());
        buf.baseBlobIdx = static_cast<std::uint16_t>(w.addBlob(bufData, level, &err));
        w.addAsset("meshes/buffer.bin", buf);
        const Bytes file = w.finish();

        auto p = ma::AssetPackage::fromBytes(file, &err);
        CHECK_MSG(p.has_value(), "L%u: %s", level, err.c_str());
        if (!p) {
            continue;
        }
        CHECK(p->assetCount() == cases.size() + 1);
        for (std::uint32_t i = 0; i < cases.size(); ++i) {
            CHECK(p->findAsset(cases[i].name) == i);
            auto img = p->loadImage(i, &err);
            CHECK_MSG(img && sameImage(*img, cases[i].image), "L%u %s: %s", level, cases[i].name, err.c_str());
            // The first loose level / the tail blob are where upstream's getBlobIndex says.
            const ma::PackageAssetDesc* a = p->asset(i);
            const std::uint32_t loose = a->numMips - a->numTailMips;
            if (loose > 0) {
                CHECK(p->blobIndex(i, 0, 0, 0) == a->baseBlobIdx && p->blobIndex(i, 0, 0, loose - 1) == a->baseBlobIdx + loose - 1u);
            }
            if (a->numTailMips > 0) {
                CHECK(p->blobIndex(i, 0, 0, a->numMips - 1) == a->tailBlobIdx);
            }
        }
        const std::uint32_t cubeIdx = p->findAsset("bc6h cube array tail");
        const ma::PackageAssetDesc* cube = p->asset(cubeIdx);
        CHECK(p->blobIndex(cubeIdx, 1, 2, 0) == cube->baseBlobIdx + (1u * 6 + 2) * 3u);
        CHECK(p->blobIndex(cubeIdx, 1, 2, 4) == cube->tailBlobIdx + (1u * 6 + 2) * 3u);
        CHECK(p->blobIndex(cubeIdx, 2, 0, 0) == ma::AssetPackage::kNoIndex && p->blobIndex(cubeIdx, 0, 6, 0) == ma::AssetPackage::kNoIndex);
        auto b = p->loadBuffer(cases.size(), &err);
        CHECK_MSG(b && *b == bufData, "buffer: %s", err.c_str());
        for (std::uint32_t i = 0; i < p->blobCount(); ++i) {
            CHECK(p->blobCrcMatches(i));
            CHECK(p->blob(i)->compression == ((level > 0 && p->blob(i)->size > 0) ? 1 : 0));
        }
        if (level > 0) {
            // A compressed blob cannot be read raw as image data: readBlob decodes, readBlobRaw does not.
            auto raw = p->readBlobRaw(0), dec = p->readBlob(0);
            CHECK(raw && dec && raw->size() != dec->size() && gd::inspect(*raw).has_value());
            // Corrupting the compressed blob makes the image load fail cleanly.
            Bytes broken = file;
            broken[p->blob(0)->offset + 12] ^= 0xff;
            broken[p->blob(0)->offset + 40] ^= 0xff;
            auto pb = ma::AssetPackage::fromBytes(broken);
            CHECK(pb && !pb->blobCrcMatches(0));
        }

        // The same bytes through the file-backed reader.
        const std::filesystem::path path = std::filesystem::temp_directory_path() / ("rl_mods_assets_" + std::to_string(level) + ".pkg");
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }
        {
            // The package keeps its file open for on-demand blob reads (as upstream): scope it before removal
            // (Windows refuses to delete an open file).
            auto pf = ma::AssetPackage::open(path, &err);
            CHECK_MSG(pf.has_value(), "open: %s", err.c_str());
            if (pf) {
                auto img = pf->loadImage(0, &err);
                CHECK(img && sameImage(*img, cases[0].image));
                auto buf2 = pf->loadBuffer(cases.size());
                CHECK(buf2 && *buf2 == bufData);
            }
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        CHECK(!ec);
    }
    std::string err;
    CHECK(!ma::AssetPackage::open("/nonexistent/relight/none.pkg", &err) && err.find("unable") != std::string::npos);

    // The writer refuses what the layout cannot address.
    ma::AssetPackageWriter w;
    const ma::TextureImage arr = makeImage(TexFormat::R8_UNORM, ma::TexDimension::Tex2D, 4, 4, 1, 3, 2, 9);
    CHECK(!w.addImage("x", arr, 3, 0, &err));
    const ma::TextureImage big = makeImage(TexFormat::R32G32B32A32_SFLOAT, ma::TexDimension::Tex2D, 1, 1, 1, 1, 1, 9);
    ma::TextureImage wrongData = big;
    wrongData.data.pop_back();
    CHECK(!w.addImage("y", wrongData, 0, 0, &err));
}

} // namespace

int main() {
    testFormats();
    testDecodePlain();
    testDecodeBlocks();
    testDdsDx10();
    testDdsLegacy();
    testDdsMalformed();
#if FUSE_RL_MODS_HAVE_CAPTURE_EXPORT
    testDdsCaptureWriter();
#else
    std::printf("note: capture/export (RL-1.8) not in this tree; its DDS cross-check is skipped\n");
#endif
    testGdeflate();
    testPackageLayout();
    testPackageRoundTrip();
    std::printf("rl_mods_assets_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
