// E06 (AP-RT-COOKED, UNI-U7-ASSET-1 render half) CPU gates. Lavapipe gates: test_cooked_assets_vk.cpp.
//
//   bcn       the BCn CPU decoder (the fallback when the device lacks textureCompressionBC) against reference blocks
//             worked out by hand from the D3D11 / Khronos Data Format rules (BC1 four- and three-colour, BC4 eight-
//             and six-value, BC5, BC7 modes 6 / 1 / 4 with rotation / 5 / reserved, BC6H modes 11 / 14 / reserved), and
//             against the cook's own decoders on the cook's encoder output (BC1 / BC4 / BC5 within 1 / 255 rounding,
//             BC7 and BC6H bit-exact), plus decode_cooked_texture layout (arrays, cubes, odd sizes)
//   fallback  format mapping, block-compressed staging sizes (UploadQueue), no-device upload refusal, the
//             layered-material library's cooked chain (MlLibrary::addCookedTexture + ml_build_mips) == the decoded
//             cooked levels, and ml_trilinear at magnification == the pool filter of decoded level 0
//   mesh      FMSH v2 (cook: meshlets + cluster DAG) -> MeshletMesh adopted as cooked (valid, same triangles as the
//             FMSH, bounds contain every decoded vertex, DAG valid) and equal in rasterised geometry to the builder's
//             output from the raw streams; FMSH v1 -> built path
//   registry  CookedAssetRegistry as the AssetRegistry's IRenderUploadSink (CPU-only GpuScene): .fusetex / .fusemat
//             / .fusemesh through VFS -> job decode -> upload; texture cook ids, late texture resolution, layered
//             GpuScene rows, MeshRegistry engine ids + meshRemap, ECS MeshAssets sync, release / unload
//   material  .fusemat through fuse_asset's reader -> fusemat_from_cooked == the renderer's own binary reader
//   hybrid    Hybrid CookedAssetBindings: cooked files recognised by the runtime readers (BC7 / BC5 .fusetex, .fusemat;
//             marker stubs still probed), uploaded through an attached CookedAssetRegistry (texture index, GpuScene
//             material row, late texture resolution by cook id), rebinding after clear() reuses the uploads
// Exit 77: the suite needs the cook library (Tools/FUSE/Cook) and it is not in this build.
#include "cooked_assets/cooked_assets_test_content.hpp"

#include <fuse/asset/asset_registry.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/cooked_assets/bcn_decode.hpp>
#include <fuse/renderer/cooked_assets/cooked_asset_registry.hpp>
#include <fuse/renderer/cooked_assets/cooked_mesh.hpp>
#include <fuse/renderer/cooked_assets/cooked_texture_gpu.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_layers/fusemat.hpp>
#include <fuse/renderer/material_layers/ml_mips.hpp>
#include <fuse/renderer/material_layers/ml_reference.hpp>
#include <fuse/renderer/scene_renderer/mesh_registry.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#if defined(FUSE_CA_HAS_HYBRID)
#include <fuse/hybrid/cooked_asset_bindings.hpp>
#endif
#if defined(FUSE_CA_HAS_COOK)
#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/bcn_encoder.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fuse;
using namespace fuse::renderer;
namespace ca = fuse::renderer::cooked_assets;
namespace ml = fuse::renderer::material_layers;

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// --- bit writer for hand-built blocks (LSB first) --------------------------------------------------------------
struct BitWriter {
    u8 bytes[16] = {};
    u32 pos = 0;
    void put(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            if ((value >> i) & 1u) {
                bytes[(pos + i) >> 3u] |= static_cast<u8>(1u << ((pos + i) & 7u));
            }
        }
        pos += count;
    }
};

u32 texel(const u8 rgba[64], u32 t, u32 c) { return rgba[t * 4u + c]; }

// ================================================================================================================
int suite_bcn() {
    // ---- BC1 four-colour: c0 = red (0xF800), c1 = blue (0x001F), indices 0, 1, 2, 3 repeating.
    {
        const u8 block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
        u8 out[64];
        ca::bc1_decode_block(block, out);
        const u32 want[4][4] = {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}};
        for (u32 t = 0; t < 16u; ++t) {
            for (u32 c = 0; c < 4u; ++c) {
                expect(texel(out, t, c) == want[t % 4u][c], "BC1 four-colour texel " + std::to_string(t));
            }
        }
    }
    // ---- BC1 three-colour (c0 <= c1): index 2 = midpoint (rounded), index 3 = opaque black (BC1_RGB).
    {
        const u8 block[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
        u8 out[64];
        ca::bc1_decode_block(block, out);
        const u32 want[4][4] = {{0, 0, 255, 255}, {255, 0, 0, 255}, {128, 0, 128, 255}, {0, 0, 0, 255}};
        for (u32 t = 0; t < 4u; ++t) {
            for (u32 c = 0; c < 4u; ++c) {
                expect(texel(out, t, c) == want[t][c], "BC1 three-colour texel " + std::to_string(t));
            }
        }
        // 565 expansion by bit replication: 0x7BEF (15, 31, 15) -> (123, 125, 123).
        const u8 grey[8] = {0xEF, 0x7B, 0x00, 0x00, 0, 0, 0, 0};
        ca::bc1_decode_block(grey, out);
        expect(texel(out, 0, 0) == 123u && texel(out, 0, 1) == 125u && texel(out, 0, 2) == 123u, "BC1 565 expansion");
    }
    // ---- BC4 eight-value (r0 > r1) and six-value (r0 <= r1) palettes; texel t takes index t % 8.
    {
        BitWriter w;
        w.put(200, 8);
        w.put(100, 8);
        for (u32 t = 0; t < 16u; ++t) {
            w.put(t % 8u, 3);
        }
        u8 v[16];
        ca::bc4_decode_block(w.bytes, v);
        const u32 want8[8] = {200, 100, 186, 171, 157, 143, 129, 114}; // round((8-i) r0 + (i-1) r1) / 7)
        for (u32 t = 0; t < 16u; ++t) {
            expect(v[t] == want8[t % 8u], "BC4 eight-value index " + std::to_string(t % 8u));
        }
        BitWriter w6;
        w6.put(50, 8);
        w6.put(250, 8);
        for (u32 t = 0; t < 16u; ++t) {
            w6.put(t % 8u, 3);
        }
        ca::bc4_decode_block(w6.bytes, v);
        const u32 want6[8] = {50, 250, 90, 130, 170, 210, 0, 255};
        for (u32 t = 0; t < 8u; ++t) {
            expect(v[t] == want6[t], "BC4 six-value index " + std::to_string(t));
        }
        // BC5 = BC4 (R) + BC4 (G), B = 0, A = 255.
        u8 bc5[16];
        std::memcpy(bc5, w.bytes, 8);
        std::memcpy(bc5 + 8, w6.bytes, 8);
        u8 rg[64];
        ca::bc5_decode_block(bc5, rg);
        for (u32 t = 0; t < 16u; ++t) {
            expect(texel(rg, t, 0) == want8[t % 8u] && texel(rg, t, 1) == want6[t % 8u] && texel(rg, t, 2) == 0u &&
                       texel(rg, t, 3) == 255u,
                   "BC5 texel " + std::to_string(t));
        }
    }
    // ---- BC7 mode 6, both p-bits 1: R0 = 0x7F|1 -> 255, R1 = 0|1 -> 1, G = B = 1, alpha 255. Index i: weight w4[i].
    {
        BitWriter w;
        w.put(1u << 6u, 7); // mode 6
        w.put(0x7F, 7);     // R0
        w.put(0x00, 7);     // R1
        w.put(0, 7);        // G0
        w.put(0, 7);        // G1
        w.put(0, 7);        // B0
        w.put(0, 7);        // B1
        w.put(0x7F, 7);     // A0
        w.put(0x7F, 7);     // A1
        w.put(1, 1);        // p0
        w.put(1, 1);        // p1
        const u32 idx[16] = {0, 15, 8, 4, 1, 2, 3, 5, 6, 7, 9, 10, 11, 12, 13, 14};
        for (u32 t = 0; t < 16u; ++t) {
            w.put(idx[t], t == 0u ? 3u : 4u);
        }
        expect(w.pos == 128u, "BC7 mode 6 block is 128 bits");
        u8 out[64];
        ca::bc7_decode_block(w.bytes, out);
        // p1 = 1 on the second endpoint: R1 = (0 << 1 | 1) = 1 (8 bits); interpolate(255, 1, w).
        auto lerp = [](u32 a, u32 b, u32 wgt) { return (a * (64u - wgt) + b * wgt + 32u) >> 6u; };
        const u32 w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
        for (u32 t = 0; t < 16u; ++t) {
            expect(texel(out, t, 0) == lerp(255u, 1u, w4[idx[t]]) && texel(out, t, 1) == lerp(1u, 1u, w4[idx[t]]) &&
                       texel(out, t, 3) == 255u,
                   "BC7 mode 6 texel " + std::to_string(t));
        }
        expect(texel(out, 0, 0) == 255u && texel(out, 1, 0) == 1u && texel(out, 2, 0) == 120u,
               "BC7 mode 6 hand values (255, 1, 120)");
    }
    // ---- BC7 mode 5 with rotation 1 (swap R and A): colour 7-bit endpoints, alpha 8-bit, 2-bit indices both.
    {
        BitWriter w;
        w.put(1u << 5u, 6); // mode 5
        w.put(1, 2);        // rotation 1: R <-> A
        w.put(0x7F, 7);     // R0 -> 255
        w.put(0x00, 7);     // R1 -> 0
        w.put(0x40, 7);     // G0 -> 0x81
        w.put(0x40, 7);     // G1
        w.put(0x00, 7);     // B0
        w.put(0x00, 7);     // B1
        w.put(0x10, 8);     // A0 = 16
        w.put(0xF0, 8);     // A1 = 240
        for (u32 t = 0; t < 16u; ++t) {
            w.put(t == 0u ? 0u : 3u, t == 0u ? 1u : 2u); // colour index: texel 0 -> 0, others 3
        }
        for (u32 t = 0; t < 16u; ++t) {
            w.put(t == 0u ? 1u : 2u, t == 0u ? 1u : 2u); // alpha index: texel 0 -> 1, others 2
        }
        expect(w.pos == 128u, "BC7 mode 5 block is 128 bits");
        u8 out[64];
        ca::bc7_decode_block(w.bytes, out);
        // G0 = 0x40 (7 bits) -> (0x40 << 1) | (0x40 >> 6) = 0x81. Alpha index 1 (w 21): (16*43 + 240*21 + 32) >> 6 = 90.
        // Rotation 1: output R = alpha value, A = red value.
        expect(texel(out, 0, 0) == 90u && texel(out, 0, 3) == 255u && texel(out, 0, 1) == 0x81u,
               "BC7 mode 5 rotation texel 0");
        // Alpha index 2 (w 43): (16*21 + 240*43 + 32) >> 6 = 167; colour index 3 -> R1 = 0.
        expect(texel(out, 5, 0) == 167u && texel(out, 5, 3) == 0u, "BC7 mode 5 rotation texel 5");
    }
    // ---- BC7 reserved mode (first byte 0): all zeros.
    {
        const u8 block[16] = {};
        u8 out[64];
        std::memset(out, 7, sizeof(out));
        ca::bc7_decode_block(block, out);
        bool zero = true;
        for (const u8 b : out) {
            zero = zero && b == 0u;
        }
        expect(zero, "BC7 reserved mode decodes to zeros");
    }
    // ---- BC6H mode 11 (one region, 10-bit raw endpoints): w = (1023, 512, 0), x = (0, 512, 1023).
    {
        BitWriter w;
        w.put(0x03, 5); // mode 11
        w.put(1023, 10); // rw
        w.put(512, 10);  // gw
        w.put(0, 10);    // bw
        w.put(0, 10);    // rx
        w.put(512, 10);  // gx
        w.put(1023, 10); // bx
        for (u32 t = 0; t < 16u; ++t) {
            w.put(t == 0u ? 0u : 15u, t == 0u ? 3u : 4u);
        }
        expect(w.pos == 128u, "BC6H mode 11 block is 128 bits");
        u16 out[64];
        ca::bc6h_decode_block(w.bytes, out, false);
        // unquantize(1023, 10) = 0xFFFF -> (0xFFFF * 31) >> 6 = 0x7BFF; unquantize(512) = (512 << 15 + 0x4000) >> 9
        // = 32800 -> 32800 * 31 >> 6 = 15887 = 0x3E0F; 0 -> 0.
        expect(out[0] == 0x7BFFu && out[1] == 0x3E0Fu && out[2] == 0u && out[3] == 0x3C00u, "BC6H mode 11 texel 0");
        expect(out[4] == 0u && out[5] == 0x3E0Fu && out[6] == 0x7BFFu && out[7] == 0x3C00u, "BC6H mode 11 texel 1");
        expect(std::fabs(ca::half_bits_to_float(0x3E0Fu) - 1.51465f) < 1e-4f, "half decode 0x3E0F");
        expect(ca::half_bits_to_float(0x7BFFu) == 65504.f && ca::half_bits_to_float(0x0001u) == std::ldexp(1.f, -24),
               "half decode max / subnormal");
    }
    // ---- BC6H mode 14 (16-bit endpoint w, 4-bit deltas, reversed high bits): w = 0x8000 exactly.
    {
        BitWriter w;
        w.put(0x0F, 5); // mode 14
        const u32 rw = 0x8000u;
        w.put(rw & 1023u, 10); // rw[9:0]
        w.put(0, 10);          // gw[9:0]
        w.put(0, 10);          // bw[9:0]
        w.put(1, 4);           // rx[3:0] = +1
        for (u32 b = 15u; b + 1u > 10u; --b) {
            w.put((rw >> b) & 1u, 1); // rw[15..10], reversed
        }
        w.put(0, 4);           // gx
        w.put(0, 6);           // gw[15..10]
        w.put(0, 4);           // bx
        w.put(0, 6);           // bw[15..10]
        for (u32 t = 0; t < 16u; ++t) {
            w.put(t == 0u ? 0u : 15u, t == 0u ? 3u : 4u);
        }
        expect(w.pos == 128u, "BC6H mode 14 block is 128 bits");
        u16 out[64];
        ca::bc6h_decode_block(w.bytes, out, false);
        // 16-bit endpoints are not unquantized: R0 = 0x8000 -> (0x8000 * 31) >> 6 = 15872 = 0x3E00; R1 = 0x8001 ->
        // (0x8001 * 31) >> 6 = 15872.
        expect(out[0] == 0x3E00u && out[1] == 0u && out[4] == 0x3E00u, "BC6H mode 14 (reversed high bits)");
    }
    // ---- BC6H reserved mode (0x13): zeros, alpha 1.
    {
        BitWriter w;
        w.put(0x13, 5);
        u16 out[64];
        ca::bc6h_decode_block(w.bytes, out, false);
        expect(out[0] == 0u && out[3] == 0x3C00u, "BC6H reserved mode");
    }

#if defined(FUSE_CA_HAS_COOK)
    // ---- the cook's encoders -> both decoders (random-ish content, 20 x 12 with padding blocks).
    const u32 w = 20u;
    const u32 h = 12u;
    std::vector<u32> words = ca_test::makeAlbedo(32u);
    cook::BcSourceImage img;
    img.width = w;
    img.height = h;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 v = words[(y * 7u % 32u) * 32u + (x * 5u + y) % 32u] ^ (x * 0x010203u * y);
            for (u32 c = 0; c < 4u; ++c) {
                img.rgba8.push_back(static_cast<u8>((v >> (8u * c)) & 255u));
            }
            for (u32 c = 0; c < 3u; ++c) {
                img.rgba16f.push_back(cook::float_to_half(static_cast<f32>(((v >> (8u * c)) & 255u) / 64.0)));
            }
            img.rgba16f.push_back(cook::float_to_half(1.f));
        }
    }
    for (const asset::BcFormat f : {asset::BcFormat::BC1, asset::BcFormat::BC4, asset::BcFormat::BC5,
                                    asset::BcFormat::BC6H, asset::BcFormat::BC7}) {
        std::vector<u8> blocks;
        std::string error;
        expect(cook::encode_bc_image(f, img, blocks, &error), std::string("cook encodes ") + asset::bc_format_name(f));
        cook::BcSourceImage ref;
        expect(cook::decode_bc_image(f, blocks.data(), blocks.size(), w, h, ref),
               std::string("cook decodes ") + asset::bc_format_name(f));
        const u32 tb = ca::bc_decoded_texel_bytes(f);
        std::vector<u8> mine(static_cast<usize>(w) * h * tb);
        expect(ca::decode_bc_level(f, blocks.data(), blocks.size(), w, h, 1u, mine.data()),
               std::string("decode_bc_level ") + asset::bc_format_name(f));
        u32 maxDiff = 0;
        for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
            for (u32 c = 0; c < 4u; ++c) {
                if (f == asset::BcFormat::BC6H) {
                    u16 a = 0;
                    std::memcpy(&a, mine.data() + i * 8u + c * 2u, 2u);
                    const u16 b = ref.rgba16f[i * 4u + c];
                    maxDiff = std::max<u32>(maxDiff, a > b ? a - b : b - a);
                } else {
                    const u32 a = mine[i * 4u + c];
                    const u32 b = ref.rgba8[i * 4u + c];
                    maxDiff = std::max<u32>(maxDiff, a > b ? a - b : b - a);
                }
            }
        }
        const bool exact = f == asset::BcFormat::BC7 || f == asset::BcFormat::BC6H;
        std::printf("bcn: %s vs cook decoder, max |diff| = %u\n", asset::bc_format_name(f), maxDiff);
        expect(exact ? maxDiff == 0u : maxDiff <= 1u, std::string("decoder agrees with the cook's for ") +
                                                          asset::bc_format_name(f));
    }

    // ---- decode_cooked_texture: array of 3 layers, odd size, full chain; layout = UploadQueue mip-major.
    {
        cook::TextureCookOptions o;
        o.format = asset::BcFormat::BC7;
        std::vector<u32> layers;
        for (u32 l = 0; l < 3u; ++l) {
            const std::vector<u32> t = ca_test::makeAlbedo(13u);
            for (u32 v : t) {
                layers.push_back(v ^ (l * 0x00102030u));
            }
        }
        // makeAlbedo(13) is 13 x 13; cook it as 13 x 13 x 3.
        asset::CookedTexture tex;
        std::string error;
        expect(ca_test::cookTexture(layers, 13u, 13u, 3u, o, tex, nullptr, &error), "cook 13x13x3 BC7: " + error);
        ca::DecodedTexture d;
        expect(ca::decode_cooked_texture(tex, d), "decode_cooked_texture");
        expect(d.levels == tex.levels.size() && d.levels == 4u && d.texelBytes == 4u, "13x13 chain has 4 levels");
        UploadImageDesc ud{};
        ud.width = 13u;
        ud.height = 13u;
        ud.layerCount = 3u;
        ud.mipLevels = d.levels;
        ud.bytesPerTexel = 4u;
        expect(UploadQueue::imageSourceBytes(ud) == d.bytes.size(), "decoded chain size == UploadQueue source bytes");
        std::vector<u32> l2;
        expect(ca::decode_cooked_level_rgba8(tex, 1u, 2u, l2) && l2.size() == 36u, "level 1 layer 2 is 6 x 6");
        bool same = true;
        for (usize i = 0; i < l2.size(); ++i) {
            u32 word = 0;
            std::memcpy(&word, d.bytes.data() + d.levelOffsets[1] + (2u * 36u + i) * 4u, 4u);
            same = same && word == l2[i];
        }
        expect(same, "decode_cooked_level_rgba8 == the matching slice of decode_cooked_texture");
    }
    return 0;
#else
    std::printf("bcn: cook library not in this build; reference blocks only\n");
    return 0;
#endif
}

// ================================================================================================================
int suite_fallback() {
    expect(ca::bc_native_format(asset::BcFormat::BC7, true) == GpuFormat::Bc7Srgb &&
               ca::bc_native_format(asset::BcFormat::BC7, false) == GpuFormat::Bc7Unorm &&
               ca::bc_native_format(asset::BcFormat::BC1, true) == GpuFormat::Bc1RgbSrgb &&
               ca::bc_native_format(asset::BcFormat::BC5, true) == GpuFormat::Bc5Unorm &&
               ca::bc_native_format(asset::BcFormat::BC4, false) == GpuFormat::Bc4Unorm &&
               ca::bc_native_format(asset::BcFormat::BC6H, false) == GpuFormat::Bc6hUfloat,
           "native BC formats (VkFormat values)");
    expect(static_cast<u32>(GpuFormat::Bc7Srgb) == 146u && static_cast<u32>(GpuFormat::Bc5Unorm) == 141u,
           "GpuFormat BC values are VkFormat values");
    expect(ca::bc_fallback_format(asset::BcFormat::BC7, true) == GpuFormat::R8G8B8A8Srgb &&
               ca::bc_fallback_format(asset::BcFormat::BC5, true) == GpuFormat::R8G8B8A8Unorm &&
               ca::bc_fallback_format(asset::BcFormat::BC6H, false) == GpuFormat::R16G16B16A16Sfloat,
           "fallback formats");
    expect(!ca::device_supports_bc_format(nullptr, GpuFormat::Bc7Unorm), "no device: no native BC");
    expect(ca::is_block_compressed(GpuFormat::Bc6hUfloat) && !ca::is_block_compressed(GpuFormat::R8G8B8A8Unorm),
           "is_block_compressed");

    // Block-compressed staging: 13 x 7, 4 mips, 2 layers of BC7 = sum over mips of ceil(w/4) ceil(h/4) 16 B x 2.
    UploadImageDesc ud{};
    ud.width = 13u;
    ud.height = 7u;
    ud.mipLevels = 4u;
    ud.layerCount = 2u;
    ud.bytesPerTexel = 16u;
    ud.blockWidth = 4u;
    ud.blockHeight = 4u;
    const usize want = (4u * 2u + 2u * 1u + 1u * 1u + 1u * 1u) * 16u * 2u;
    expect(UploadQueue::imageSourceBytes(ud) == want, "BC7 staging source bytes");
    expect(UploadQueue::imageStagingBytes(ud) >= want && UploadQueue::imageStagingBytes(ud) % 16u == 0u,
           "BC7 staged chain aligned to the block size");
    UploadImageDesc bad = ud;
    bad.blockWidth = 0u;
    expect(UploadQueue::imageSourceBytes(bad) == 0u, "zero block size rejected");

#if defined(FUSE_CA_HAS_COOK)
    // No allocator / queue: refused, nothing allocated.
    asset::CookedTexture tex;
    cook::TextureCookOptions o;
    std::string error;
    expect(ca_test::cookTexture(ca_test::makeAlbedo(64u), 64u, 64u, 1u, o, tex, nullptr, &error), "cook albedo: " + error);
    ca::CookedTextureGpu gpu{};
    expect(!ca::upload_cooked_texture(ca::CookedTextureUploadDesc{}, tex, gpu, &error) && gpu.image.image == nullptr,
           "upload without allocator refused");

    // The layered-material library: cooked chain == decoded cooked levels (not a box filter).
    ml::MlLibrary lib;
    auto shared = std::make_shared<asset::CookedTexture>(tex);
    const u32 t = lib.addCookedTexture("albedo", shared);
    expect(t != ml::kMlNoTexture && lib.cookedTexture(t) != nullptr, "addCookedTexture");
    expect((lib.textures()[t].flags & ml::kMlTexSrgb) != 0u, "sRGB BC7 -> kMlTexSrgb");
    ml::MlMipChains chains;
    ml::ml_build_mips(lib, chains);
    expect(chains.levelCount[t] == tex.levels.size(), "chain length == cooked levels");
    bool same = true;
    for (u32 l = 0; l < chains.levelCount[t]; ++l) {
        std::vector<u32> dec;
        same = same && ca::decode_cooked_level_rgba8(tex, l, 0u, dec);
        const ml::MlMipLevel& lv = chains.levels[chains.firstLevel[t] + l];
        for (u32 i = 0; i < lv.width * lv.height && same; ++i) {
            same = chains.texels[lv.offset + i] == dec[i];
        }
    }
    expect(same, "ml_build_mips of a cooked texture == its decoded levels");
    // Magnification: trilinear == the pool bilinear of decoded level 0.
    const ml::MlView v = lib.view();
    const ml::MlF4 a = ml::ml_trilinear(chains, t, lib.textures()[t].flags, ml::MlF2{0.3f, 0.7f}, ml::MlF2{1e-4f, 0.f},
                                        ml::MlF2{0.f, 1e-4f});
    const ml::MlF4 b = ml::ml_bilinear(v, lib.textures()[t], ml::MlF2{0.3f, 0.7f});
    expect(a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w, "trilinear at magnification == pool bilinear");
    // A BC5 normal map in the pool: (r, g, 0, 1).
    cook::TextureCookOptions no;
    no.normal_map = true;
    no.format = asset::BcFormat::BC5;
    asset::CookedTexture nt;
    expect(ca_test::cookTexture(ca_test::makeNormalMap(64u), 64u, 64u, 1u, no, nt, nullptr, &error), "cook normal: " + error);
    {
        // The cooked BC5 normal map decodes to the source's X / Y (the cook maps RGB8 128 -> 0, BC5 error).
        const std::vector<u32> srcN = ca_test::makeNormalMap(64u);
        std::vector<u32> dec;
        expect(ca::decode_cooked_level_rgba8(nt, 0u, 0u, dec), "decode BC5 level 0");
        u32 worst = 0;
        for (usize i = 0; i < dec.size(); ++i) {
            for (u32 c = 0; c < 2u; ++c) {
                const u32 a = (dec[i] >> (8u * c)) & 255u;
                const u32 b = (srcN[i] >> (8u * c)) & 255u;
                worst = std::max(worst, a > b ? a - b : b - a);
            }
        }
        std::printf("fallback: BC5 normal map level 0 max |decoded - source| = %u / 255 (X, Y)\n", worst);
        expect(worst <= 6u, "BC5 normal map keeps X / Y");
    }
    const u32 n = lib.addCookedTexture("normal", std::make_shared<asset::CookedTexture>(nt));
    expect(n != ml::kMlNoTexture && (lib.textures()[n].flags & ml::kMlTexSrgb) == 0u, "BC5 normal map is linear");
    expect(((lib.texels()[lib.textures()[n].offset] >> 16u) & 255u) == 0u &&
               (lib.texels()[lib.textures()[n].offset] >> 24u) == 255u,
           "BC5 pool texel is (r, g, 0, 255)");
    return 0;
#else
    std::printf("fallback: cook library not in this build; format mapping only\n");
    return 0;
#endif
}

// ================================================================================================================
#if defined(FUSE_CA_HAS_COOK)
using Tri = std::array<u32, 3>;

/// Triangle (FMSH vertex index triples, rotated so the smallest index is first: winding kept), sorted.
Tri canonical(u32 a, u32 b, u32 c) {
    if (a <= b && a <= c) {
        return Tri{a, b, c};
    }
    if (b <= a && b <= c) {
        return Tri{b, c, a};
    }
    return Tri{c, a, b};
}

std::vector<Tri> sourceTriangles(const asset::CookedMesh& m) {
    std::vector<Tri> out;
    for (usize i = 0; i + 2u < m.indices.size(); i += 3u) {
        out.push_back(canonical(m.indices[i], m.indices[i + 1u], m.indices[i + 2u]));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The meshlet mesh's triangles in FMSH vertex indices (through VSRC).
std::vector<Tri> meshletTriangles(const geometry::MeshletMesh& m) {
    std::vector<Tri> out;
    for (const geometry::MeshletRecord& r : m.meshlets) {
        for (u32 t = 0; t < r.triangle_count; ++t) {
            const u32 packed = m.meshlet_triangles[r.triangle_offset + t];
            u32 v[3];
            for (u32 c = 0; c < 3u; ++c) {
                v[c] = m.source_vertices[m.meshlet_vertices[r.vertex_offset + ((packed >> (8u * c)) & 255u)]];
            }
            out.push_back(canonical(v[0], v[1], v[2]));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Largest |decoded - source| position error over the cooked vertices, in quantisation steps.
f64 decodeErrorSteps(const geometry::MeshletMesh& m, const asset::CookedMesh& src) {
    geometry::DecodedVertices d;
    geometry::decode_vertices(m, d);
    f64 worst = 0.0;
    for (u32 v = 0; v < m.vertex_count(); ++v) {
        for (u32 a = 0; a < 3u; ++a) {
            const f64 e = std::fabs(static_cast<f64>(d.positions[v * 3u + a]) - src.positions[m.source_vertices[v] * 3u + a]);
            worst = std::max(worst, e / m.quant.step[a]);
        }
    }
    return worst;
}
#endif

int suite_mesh() {
#if defined(FUSE_CA_HAS_COOK)
    if (!cook::mesh_meshlets_available()) {
        std::printf("SKIP: the cook's meshlet builder is not in this build\n");
        return 77;
    }
    const asset::CookedMesh source = ca_test::makeSphere(24u, 32u, 0.5f);
    asset::CookedMesh v2;
    std::string error;
    expect(ca_test::cookMesh(source, true, true, v2, nullptr, &error), "cook FMSH v2 with meshlets + DAG: " + error);
    expect(!v2.meshlets.empty() && !v2.cluster_dag.empty(), "FMSH v2 carries meshlets and a DAG");
    {
        f64 worstT = 0.0;
        f64 worstN = 0.0;
        bool signs = v2.tangents.size() == source.tangents.size();
        for (usize i = 0; signs && i < source.tangents.size(); ++i) {
            if (i % 4u == 3u) {
                signs = signs && v2.tangents[i] == source.tangents[i];
            } else {
                worstT = std::max(worstT, std::fabs(static_cast<f64>(v2.tangents[i]) - source.tangents[i]));
            }
        }
        for (usize i = 0; i < source.normals.size() && v2.normals.size() == source.normals.size(); ++i) {
            worstN = std::max(worstN, std::fabs(static_cast<f64>(v2.normals[i]) - source.normals[i]));
        }
        std::printf("mesh: FMSH v2 round trip: tangent max |diff| %.2e (signs %s), normal %.2e\n", worstT,
                    signs ? "kept" : "CHANGED", worstN);
        expect(signs && worstT < 1e-3 && worstN < 1e-3, "FMSH v2 keeps the tangent frame");
    }

    ca::CookedMeshletResult adopted;
    expect(ca::cooked_mesh_to_meshlets(v2, adopted, &error), "convert FMSH v2: " + error);
    expect(adopted.path == ca::CookedMeshPath::Adopted, "FMSH v2 meshlets adopted (" + adopted.note + ")");
    expect(adopted.hasDag && adopted.dag.cluster_count() == v2.cluster_dag.cluster_count(), "DAG adopted");
    std::string why;
    expect(geometry::validate_meshlet_mesh(adopted.mesh, &why), "adopted mesh valid: " + why);
    expect(geometry::dag::validate_cluster_dag(adopted.mesh, adopted.dag, &why), "adopted DAG valid: " + why);
    expect(adopted.mesh.meshlets.size() == v2.meshlets.meshlets.size(), "one meshlet per cooked meshlet");

    // Builder output from the same (FMSH-stored) streams without the sections.
    ca::CookedMeshletResult built;
    asset::CookedMesh streams = v2;
    streams.meshlets = asset::MeshletTable{};
    streams.cluster_dag = asset::ClusterDagTable{};
    expect(ca::cooked_mesh_to_meshlets(streams, built, &error), "convert streams: " + error);
    expect(built.path == ca::CookedMeshPath::Built && !built.hasDag && built.note == "no meshlet table",
           "no meshlet table -> built");

    const std::vector<Tri> sourceTris = sourceTriangles(source);
    expect(adopted.mesh.source_vertices.size() == adopted.mesh.vertex_count(), "adopted mesh keeps VSRC");
    expect(meshletTriangles(adopted.mesh) == sourceTris, "adopted meshlets hold exactly the FMSH triangles (winding kept)");
    expect(meshletTriangles(built.mesh) == sourceTris, "built meshlets hold exactly the FMSH triangles");
    const f64 adoptedErr = decodeErrorSteps(adopted.mesh, source);
    const f64 builtErr = decodeErrorSteps(built.mesh, source);
    std::printf("mesh: decoded position error %.3f (adopted) / %.3f (built) quantisation steps\n", adoptedErr, builtErr);
    expect(adoptedErr <= 0.5 && builtErr <= 0.5, "decoded positions within half a quantisation step");
    const bool identical = geometry::meshlet_mesh_equal(adopted.mesh, built.mesh);
    expect(identical, "adopted cooked meshlets == the WP-1.2 builder output from the same streams (bit for bit)");
    if (!identical) {
        const geometry::MeshletMesh& a = adopted.mesh;
        const geometry::MeshletMesh& b = built.mesh;
        std::printf("mesh: differs in:%s%s%s%s%s%s%s%s\n", a.flags != b.flags ? " flags" : "",
                    a.meshlet_vertices != b.meshlet_vertices ? " MVRT" : "", a.meshlet_triangles != b.meshlet_triangles ? " MTRI" : "",
                    a.positions != b.positions ? " VPOS" : "", a.normals != b.normals ? " VNRM" : "",
                    a.tangents != b.tangents ? " VTAN" : "", a.uvs != b.uvs ? " VUV0" : "",
                    a.source_vertices != b.source_vertices ? " VSRC" : "");
    }
    std::printf("mesh: %zu meshlets, DAG %u clusters / %zu groups; adopted == builder output bit for bit: %s\n",
                adopted.mesh.meshlets.size(), adopted.dag.cluster_count(), adopted.dag.groups.size(),
                identical ? "yes" : "no");

    // Every decoded vertex inside its meshlet's sphere and AABB.
    geometry::DecodedVertices d;
    geometry::decode_vertices(adopted.mesh, d);
    bool inside = true;
    for (const geometry::MeshletRecord& r : adopted.mesh.meshlets) {
        for (u32 i = 0; i < r.vertex_count; ++i) {
            const u32 vtx = adopted.mesh.meshlet_vertices[r.vertex_offset + i];
            f64 d2 = 0.0;
            for (u32 a = 0; a < 3u; ++a) {
                const f32 x = d.positions[vtx * 3u + a];
                inside = inside && x >= r.aabb_min[a] && x <= r.aabb_max[a];
                d2 += (static_cast<f64>(x) - r.center[a]) * (static_cast<f64>(x) - r.center[a]);
            }
            inside = inside && d2 <= static_cast<f64>(r.radius) * r.radius;
        }
    }
    expect(inside, "adopted bounds contain every decoded vertex");

    // Without tangents the adopt path is refused (the builder generates them).
    asset::CookedMesh noTangents = v2;
    noTangents.tangents.clear();
    ca::CookedMeshletResult nt;
    expect(ca::cooked_mesh_to_meshlets(noTangents, nt, &error) && nt.path == ca::CookedMeshPath::Built &&
               nt.note == "no tangent stream",
           "missing tangents -> built path");
    // Invalid input.
    asset::CookedMesh empty;
    expect(!ca::cooked_mesh_to_meshlets(empty, nt, &error), "empty mesh refused");
    return 0;
#else
    std::printf("SKIP: cook library not in this build\n");
    return 77;
#endif
}

// ================================================================================================================
#if defined(FUSE_CA_HAS_COOK)
fs::path scratchDir(const char* suite) {
    const fs::path dir = fs::path(FUSE_CA_TMP_DIR) / suite;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, const std::vector<u8>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct SchedulerScope {
    explicit SchedulerScope(u32 workers) {
        jobs::JobScheduler::instance().shutdown();
        jobs::JobScheduler::instance().initialize(workers);
    }
    ~SchedulerScope() { jobs::JobScheduler::instance().shutdown(); }
};

ml::FuseMat testMaterial() {
    ml::FuseMat m{};
    m.name = "test/cooked/sphere";
    m.albedo[0] = 1.f;
    m.albedo[1] = 1.f;
    m.albedo[2] = 1.f;
    m.roughness = 0.6f;
    m.textures.albedo = "sphere_albedo";
    m.textures.normal = "sphere_normal";
    m.shading = ml::FuseMatShading::Cloth;
    return m;
}
#endif

int suite_registry() {
#if defined(FUSE_CA_HAS_COOK)
    if (!cook::mesh_meshlets_available()) {
        std::printf("SKIP: the cook's meshlet builder is not in this build\n");
        return 77;
    }
    const fs::path dir = scratchDir("registry");
    std::string error;
    std::vector<u8> albedoBytes;
    std::vector<u8> normalBytes;
    std::vector<u8> meshBytes;
    asset::CookedTexture albedo;
    asset::CookedTexture normal;
    asset::CookedMesh mesh;
    cook::TextureCookOptions ao;
    cook::TextureCookOptions no;
    no.normal_map = true;
    no.format = asset::BcFormat::BC5;
    expect(ca_test::cookTexture(ca_test::makeAlbedo(64u), 64u, 64u, 1u, ao, albedo, &albedoBytes, &error), error);
    expect(ca_test::cookTexture(ca_test::makeNormalMap(64u), 64u, 64u, 1u, no, normal, &normalBytes, &error), error);
    expect(ca_test::cookMesh(ca_test::makeSphere(12u, 16u, 0.5f), true, true, mesh, &meshBytes, &error), error);
    writeFile(dir / "tex" / "sphere_albedo.fusetex", albedoBytes);
    writeFile(dir / "tex" / "sphere_normal.fusetex", normalBytes);
    writeFile(dir / "mesh" / "sphere.fusemesh", meshBytes);
    writeFile(dir / "mat" / "sphere.fusemat", ml::write_fusemat_binary(testMaterial()));

    gpu_scene::GpuScene scene;
    gpu_scene::GpuSceneDesc sd{};
    expect(scene.init(sd) && !scene.gpuEnabled(), "CPU-only GpuScene");
    scene_renderer::MeshRegistry meshes;
    ca::CookedAssetRegistry cooked;
    ca::CookedAssetRegistryDesc cd{};
    cd.scene = &scene;
    cd.meshes = &meshes;
    cd.firstMaterialRow = 16u;
    cd.materialCapacity = 8u;
    expect(cooked.init(cd), "CookedAssetRegistry init (CPU-only)");

    SchedulerScope scheduler(2);
    io::VirtualFileSystem vfs;
    vfs.mount(io::MountKind::Game, dir.string(), "game:");
    ecs::Registry world;
    world.init(16);
    {
        asset::AssetRegistry assets(vfs);
        // The material first: its texture ids resolve once the textures arrive.
        const asset::AssetId mat = assets.acquire("game:/mat/sphere.fusemat");
        expect(assets.pumpUntilIdle(cooked, 20000), "material loads");
        expect(assets.state(mat) == asset::AssetLoadState::Ready, "material Ready: " + assets.error(mat));
        const u32 row = cooked.materialRow(mat);
        expect(row == 16u, "first material row = firstMaterialRow");
        expect(cooked.stats().unresolvedSlots == 2u, "two texture slots pending");
        const ml::MlMaterial* mm = cooked.tableMaterial(cooked.materialTableIndex(row));
        expect(mm != nullptr && mm->albedoTex == ml::kMlNoTexture, "pending albedo slot reads as no texture");

        const asset::AssetId albedoId = assets.acquire("game:/tex/sphere_albedo.fusetex");
        const asset::AssetId normalId = assets.acquire("game:/tex/sphere_normal.fusetex");
        const asset::AssetId meshId = assets.acquire("game:/mesh/sphere.fusemesh");
        expect(assets.pumpUntilIdle(cooked, 20000), "textures + mesh load");
        expect(assets.state(albedoId) == asset::AssetLoadState::Ready && assets.state(normalId) == asset::AssetLoadState::Ready &&
                   assets.state(meshId) == asset::AssetLoadState::Ready,
               "all Ready: " + assets.error(albedoId) + assets.error(normalId) + assets.error(meshId));
        expect(cooked.stats().textures == 2u && cooked.stats().materials == 1u && cooked.stats().meshes == 1u,
               "stats: 2 textures, 1 material, 1 mesh");
        expect(cooked.stats().unresolvedSlots == 0u, "texture slots resolved late");
        const u32 ta = cooked.findTexture("sphere_albedo");
        const u32 tn = cooked.findTexture("game:/tex/sphere_normal.fusetex");
        expect(ta != ml::kMlNoTexture && tn != ml::kMlNoTexture && ta != tn, "cook ids: stem and virtual path");
        expect(cooked.findTexture("game:/tex/sphere_albedo") == ta, "cook id: path without extension");
        mm = cooked.tableMaterial(cooked.materialTableIndex(row));
        expect(mm != nullptr && mm->albedoTex == ta && mm->normalTex == tn, "material slots -> table indices");
        expect(cooked.tableTextures()[ta].width == 64u && (cooked.tableTextures()[ta].flags & ml::kMlTexSrgb) != 0u &&
                   (cooked.tableTextures()[tn].flags & ml::kMlTexSrgb) == 0u,
               "table rows: size, sRGB albedo, linear normal");
        expect(assets.gpuResource(mat) == u64{row} + 1u && assets.gpuResource(albedoId) == u64{ta} + 1u,
               "gpu_resource: row + 1 / table index + 1");
        expect(cooked.commit() && !cooked.dirty(), "commit (CPU-only: rows written)");

        // GpuScene row: layered, table index in padding, cloth shading model.
        const gpu_scene::TableBytes mats = scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
        gpu_scene::GpuMaterial g{};
        expect(MaterialLayout::fetchRow(mats.data, usize{mats.count} * mats.stride, row, g), "material row present");
        expect(gpu_scene::gpu_material_layered(g) && gpu_scene::gpu_material_layered_index(g) == 0u &&
                   g.shadingModel == static_cast<u32>(ShadingModel::Cloth) && std::fabs(g.roughnessEmissive.x - 0.6f) < 1e-6f,
               "GpuScene row layered, index 0, cloth, roughness 0.6");

        // Mesh: engine id from the asset id, meshRemap after flush.
        const u32 engine = cooked.meshEngineId(meshId);
        expect(engine == meshes.findAsset(meshId.value) && engine >= scene_renderer::MeshRegistry::kFirstAssetEngineId,
               "mesh engine id = MeshRegistry::engineIdForAsset");
        expect(cooked.meshData(meshId) != nullptr && cooked.meshData(meshId)->path == ca::CookedMeshPath::Adopted &&
                   cooked.meshData(meshId)->hasDag,
               "FMSH v2 meshlets + DAG adopted");
        expect(meshes.flush(scene) == 1u && meshes.gpuMesh(engine) != scene_renderer::MeshRegistry::kInvalidMesh,
               "mesh uploaded, remap set");

        // ECS: MeshAssets -> Mesh.
        const ecs::EntityID e = world.create();
        world.add<ecs::Transform>(e, ecs::Transform{});
        world.add<ecs::Mesh>(e, ecs::Mesh{});
        ecs::MeshAssets ma{};
        ma.mesh = meshId;
        ma.material = mat;
        world.add<ecs::MeshAssets>(e, ma);
        expect(cooked.syncEntities(world) == 1u, "one entity synced");
        const ecs::Mesh* m = world.get<ecs::Mesh>(e);
        expect(m != nullptr && m->vertex_buffer.index() == engine && m->material_id == row &&
                   m->aabb_max.x > 0.49f && m->aabb_min.y < -0.49f,
               "Mesh: engine id, material row, cooked bounds");
        expect(cooked.syncEntities(world) == 0u, "second sync changes nothing");
        expect(meshes.remap()[m->vertex_buffer.index()] == meshes.gpuMesh(engine), "meshRemap[engine id] = mesh row");

        // Unload: the material row resets, textures and meshes go, the entity loses its mesh.
        assets.unloadAll();
        assets.drainRenderUploads(cooked);
        expect(cooked.stats().textures == 0u && cooked.stats().materials == 0u && cooked.stats().meshes == 0u,
               "everything released");
        expect(cooked.findTexture("sphere_albedo") == ml::kMlNoTexture, "cook ids dropped");
        expect(cooked.syncEntities(world) == 1u && !world.get<ecs::Mesh>(e)->vertex_buffer.isValid(),
               "entity mesh cleared after unload");
        const gpu_scene::TableBytes mats2 = scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
        expect(MaterialLayout::fetchRow(mats2.data, usize{mats2.count} * mats2.stride, row, g) && !gpu_scene::gpu_material_layered(g),
               "material row reset to the default material");
    }
    vfs.waitIdle(5000);
    cooked.destroy();
    fs::remove_all(dir);
    return 0;
#else
    std::printf("SKIP: cook library not in this build\n");
    return 77;
#endif
}

// ================================================================================================================
int suite_material() {
    ml::FuseMat m{};
    m.name = "rock/alpine/granite_a";
    m.shading = ml::FuseMatShading::Foliage;
    m.category = ml::FuseMatCategory::Stone;
    m.wind = ml::FuseMatWind::Leaves;
    m.albedo[0] = 0.3f;
    m.albedo[1] = 0.35f;
    m.albedo[2] = 0.4f;
    m.textures.albedo = "granite_albedo";
    m.textures.normal = "granite_normal";
    m.triplanar = true;
    m.stochastic = true;
    m.macroStrength = 0.25f;
    m.detail.albedo = "grain_albedo";
    m.detailStrength = 0.5f;
    ml::FuseMatLayer moss{};
    moss.name = "moss";
    moss.mask = ml::kMlMaskSlopeUp;
    moss.albedo[1] = 0.8f;
    moss.textures.albedo = "moss_albedo";
    m.layers.push_back(moss);
    ml::FuseMatLayer wet{};
    wet.name = "wet";
    wet.mode = ml::kMlLayerWet;
    wet.albedo[0] = 0.6f;
    wet.albedo[1] = 0.6f;
    wet.albedo[2] = 0.6f;
    wet.roughness = 0.1f;
    m.layers.push_back(wet);
    m.proceduralFunction = 7u;
    m.proceduralParams = {1.f, 2.f, 3.f};
    expect(ml::validate_fusemat(m).ok, "test material valid");
    const std::vector<u8> bytes = ml::write_fusemat_binary(m);
    asset::CookedMaterial runtime;
    std::string error;
    expect(asset::read_cooked_material(bytes.data(), bytes.size(), runtime, &error), "fuse_asset reads it: " + error);
    const ml::FuseMat back = ca::fusemat_from_cooked(runtime);
    expect(back == m, "fusemat_from_cooked(fuse_asset reader) == the renderer's FuseMat");
    ml::FuseMat renderer;
    expect(ml::read_fusemat_binary(bytes.data(), bytes.size(), renderer).ok && renderer == back,
           "== the renderer's binary reader");
    expect(ca::gpu_shading_model(ml::FuseMatShading::Foliage) == static_cast<u32>(ShadingModel::SubsurfaceSSS) &&
               ca::gpu_shading_model(ml::FuseMatShading::Unlit) == static_cast<u32>(ShadingModel::Emissive) &&
               ca::gpu_shading_model(ml::FuseMatShading::DefaultLit) == static_cast<u32>(ShadingModel::Opaque),
           "shading model mapping");

    // Direct API: a material whose textures come later resolves when they are added (CPU-only registry).
    gpu_scene::GpuScene scene;
    expect(scene.init(gpu_scene::GpuSceneDesc{}), "CPU GpuScene");
    scene_renderer::MeshRegistry meshes;
    ca::CookedAssetRegistry reg;
    ca::CookedAssetRegistryDesc cd{};
    cd.scene = &scene;
    cd.meshes = &meshes;
    cd.firstMaterialRow = 4u;
    cd.materialCapacity = 2u;
    expect(reg.init(cd), "registry");
    const u32 row = reg.addMaterial(m, &error);
    expect(row == 4u && reg.stats().unresolvedSlots == 4u, "4 pending slots (albedo, normal, detail, moss)");
    const u32 t = reg.addRgba8Texture("granite_albedo", 8u, 8u, true, std::vector<u32>(64u, 0xFF808080u));
    expect(t != ml::kMlNoTexture && reg.stats().unresolvedSlots == 3u, "albedo resolved on arrival");
    expect(reg.tableMaterial(0)->albedoTex == t && reg.tableTextures()[t].mean[3] == 1.f, "slot + mean");
    expect(reg.addTextureAlias("moss_albedo", t) && reg.tableMaterial(0)->layers[0].albedoTex == t, "alias resolves");
    expect(reg.addMaterial(m) == 5u && reg.addMaterial(m) == ca::CookedAssetRegistry::kInvalid, "capacity enforced");
    ml::FuseMat invalid = m;
    invalid.roughness = 3.f;
    expect(reg.addMaterial(invalid, &error) == ca::CookedAssetRegistry::kInvalid && !error.empty(), "invalid refused");
    return 0;
}

// ================================================================================================================
int suite_hybrid() {
#if defined(FUSE_CA_HAS_HYBRID) && defined(FUSE_CA_HAS_COOK)
    const fs::path dir = scratchDir("hybrid");
    std::string error;
    std::vector<u8> albedoBytes;
    std::vector<u8> normalBytes;
    asset::CookedTexture albedo;
    asset::CookedTexture normal;
    cook::TextureCookOptions ao;
    cook::TextureCookOptions no;
    no.normal_map = true;
    no.format = asset::BcFormat::BC5;
    expect(ca_test::cookTexture(ca_test::makeAlbedo(32u), 32u, 32u, 1u, ao, albedo, &albedoBytes, &error), error);
    expect(ca_test::cookTexture(ca_test::makeNormalMap(32u), 32u, 32u, 1u, no, normal, &normalBytes, &error), error);
    writeFile(dir / "hyb_albedo.fusetex", albedoBytes);
    writeFile(dir / "hyb_normal.fusetex", normalBytes);
    ml::FuseMat mat = testMaterial();
    mat.textures.albedo = "hyb_albedo";
    mat.textures.normal = "hyb_normal";
    writeFile(dir / "hyb.fusemat", ml::write_fusemat_binary(mat));
    {
        std::ofstream stub(dir / "stub.fusetex");
        stub << "FUSETEX_STUB\n";
    }
    gpu_scene::GpuScene scene;
    expect(scene.init(gpu_scene::GpuSceneDesc{}), "CPU GpuScene");
    scene_renderer::MeshRegistry meshes;
    ca::CookedAssetRegistry reg;
    ca::CookedAssetRegistryDesc cd{};
    cd.scene = &scene;
    cd.meshes = &meshes;
    cd.firstMaterialRow = 8u;
    cd.materialCapacity = 4u;
    expect(reg.init(cd), "registry");

    hybrid::CookedAssetBindings bindings;
    bindings.bindMaterial((dir / "hyb.fusemat").string(), 1u); // before the textures: resolves late
    bindings.attachRegistry(&reg);
    bindings.bindMaterial((dir / "hyb_albedo.fusetex").string(), 2u);
    bindings.bindMaterial((dir / "hyb_normal.fusetex").string(), 3u);
    bindings.bindMaterial((dir / "stub.fusetex").string(), 4u);
    const auto m1 = bindings.findMaterial(1u);
    const auto m2 = bindings.findMaterial(2u);
    const auto m3 = bindings.findMaterial(3u);
    const auto m4 = bindings.findMaterial(4u);
    expect(m1 && m1->headerKind == hybrid::CookedHeaderKind::MaterialFusemat && m1->uploaded && m1->gpuMaterialRow == 8u,
           ".fusemat recognised by the reader and uploaded (row 8)");
    expect(m2 && m2->headerKind == hybrid::CookedHeaderKind::TextureBc7 && m2->uploaded &&
               m2->textureIndex == reg.findTexture("hyb_albedo"),
           "BC7 .fusetex uploaded under its stem");
    expect(m3 && m3->headerKind == hybrid::CookedHeaderKind::TextureBcn && m3->uploaded, "BC5 .fusetex uploaded");
    expect(m4 && m4->headerKind == hybrid::CookedHeaderKind::TextureStub && !m4->uploaded,
           "marker stub still probed, not uploaded");
    expect(bindings.uploadedMaterialCount() == 3u && bindings.bc7MaterialCount() == 1u, "counts");
    const ml::MlMaterial* rm = reg.tableMaterial(reg.materialTableIndex(8u));
    expect(rm != nullptr && rm->albedoTex == m2->textureIndex && rm->normalTex == m3->textureIndex,
           "material slots resolved once the textures were bound");
    expect(reg.tableTextures()[m3->textureIndex].flags == ml::kMlTexRg, "BC5 table row reads B / A as 1");
    // Rebinding after clear() reuses the uploads.
    bindings.clear();
    bindings.bindMaterial((dir / "hyb.fusemat").string(), 1u);
    bindings.bindMaterial((dir / "hyb_albedo.fusetex").string(), 2u);
    expect(reg.stats().materials == 1u && reg.stats().textures == 2u && bindings.findMaterial(1u)->gpuMaterialRow == 8u,
           "rebinding reuses the uploaded rows / textures");
    bindings.attachRegistry(nullptr);
    reg.destroy();
    fs::remove_all(dir);
    return 0;
#else
    std::printf("SKIP: Hybrid or the cook library not in this build\n");
    return 77;
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    auto run = [&](const char* name, int (*fn)()) {
        if (suite == name || suite == "all") {
            const int r = fn();
            if (r == 77 && suite != "all") {
                rc = 77;
            }
        }
    };
    run("bcn", suite_bcn);
    run("fallback", suite_fallback);
    run("mesh", suite_mesh);
    run("registry", suite_registry);
    run("material", suite_material);
    run("hybrid", suite_hybrid);
    if (g_failures != 0) {
        std::printf("fuse_cooked_assets_cpu %s: %d failure(s)\n", suite.c_str(), g_failures);
        return 1;
    }
    std::printf("fuse_cooked_assets_cpu %s: %s\n", suite.c_str(), rc == 77 ? "skipped" : "passed");
    return rc;
}
