// Asset plan W0.3 gates (docs/plans/FUSE_ASSET_PLAN.md §1.3, §6 Wave 0): real BC1 / BC4 / BC5 / BC6H /
// BC7 encoding in the texture cook, per-format PSNR thresholds, normal maps → BC5, texture arrays and
// cube maps, and the version-1 `.fusetex` container staying byte-identical for plain BC7.
//
// Every PSNR is measured on blocks decoded by an independent decoder when one is compiled in
// (FUSE_COOK_TEST_HAS_BC_ORACLE: the Relight RL-3.3 decoder, itself checked against bcdec), and the
// cook's own decoders are required to agree with it.
#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/bcn_encoder.hpp>
#include <fuse/cook/ktx2.hpp>
#include <fuse/cook/texture_cook.hpp>

#if defined(FUSE_COOK_TEST_HAS_BC_ORACLE)
#include <fuse/relight/mods/assets/texture_decode.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace fuse;
using namespace fuse::cook;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string temp_path(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_asset_texture_bcn";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

std::vector<u8> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Deterministic "natural" test image: smooth gradients, a sinusoidal pattern, hard edges and
/// small-scale noise, with an alpha ramp.
std::vector<u8> make_image(u32 w, u32 h, u32 seed) {
    std::vector<u8> rgba(static_cast<usize>(w) * h * 4u);
    u32 state = 0x9E3779B9u ^ seed;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            const f32 noise = static_cast<f32>((state >> 24) & 0xFu) - 7.5f;
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
            f32 r = 40.f + 170.f * fx + 20.f * std::sin(fy * 12.f + static_cast<f32>(seed));
            f32 g = 60.f + 120.f * fy + 25.f * std::cos(fx * 9.f);
            f32 b = 90.f + 60.f * std::sin((fx + fy) * 7.f);
            if ((x / 16u + y / 16u + seed) % 3u == 0u) { // hard-edged patches
                r = 230.f - r * 0.3f;
                b = 30.f;
            }
            const f32 a = 255.f * fx;
            const f32 values[4] = {r + noise, g + noise * 0.5f, b - noise, a};
            for (u32 c = 0; c < 4u; ++c) {
                rgba[(static_cast<usize>(y) * w + x) * 4u + c] =
                    static_cast<u8>(std::lround(std::clamp(values[c], 0.f, 255.f)));
            }
        }
    }
    return rgba;
}

/// Height field → tangent-space normal map (OpenGL +Y), RGB8.
std::vector<u8> make_normal_map(u32 w, u32 h) {
    std::vector<f32> height(static_cast<usize>(w) * h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
            height[static_cast<usize>(y) * w + x] =
                0.5f * std::sin(fx * 6.2831853f * 3.f) * std::cos(fy * 6.2831853f * 2.f) + 0.2f * fx;
        }
    }
    std::vector<u8> rgba(static_cast<usize>(w) * h * 4u);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            auto at = [&](s32 px, s32 py) {
                px = std::clamp(px, 0, static_cast<s32>(w) - 1);
                py = std::clamp(py, 0, static_cast<s32>(h) - 1);
                return height[static_cast<usize>(py) * w + static_cast<usize>(px)];
            };
            const f32 dx = (at(static_cast<s32>(x) + 1, static_cast<s32>(y)) - at(static_cast<s32>(x) - 1, static_cast<s32>(y))) * 4.f;
            const f32 dy = (at(static_cast<s32>(x), static_cast<s32>(y) + 1) - at(static_cast<s32>(x), static_cast<s32>(y) - 1)) * 4.f;
            f32 n[3] = {-dx, -dy, 1.f};
            const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (u32 d = 0; d < 3u; ++d) {
                rgba[(static_cast<usize>(y) * w + x) * 4u + d] =
                    static_cast<u8>(std::lround((n[d] / len * 0.5f + 0.5f) * 255.f));
            }
            rgba[(static_cast<usize>(y) * w + x) * 4u + 3u] = 255u;
        }
    }
    return rgba;
}

/// HDR test image (linear RGB, 0 .. ~40, a sky-like gradient with a bright sun disc), as halves.
std::vector<u16> make_hdr_image(u32 w, u32 h) {
    std::vector<u16> rgba(static_cast<usize>(w) * h * 4u);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
            const f32 dx = fx - 0.7f;
            const f32 dy = fy - 0.3f;
            const f32 sun = std::exp(-(dx * dx + dy * dy) * 200.f) * 40.f;
            const f32 rgb[3] = {0.2f + 0.8f * fy + sun, 0.4f + 0.6f * fy + sun * 0.9f, 1.2f - 0.5f * fy + sun * 0.7f};
            for (u32 c = 0; c < 3u; ++c) {
                rgba[(static_cast<usize>(y) * w + x) * 4u + c] = float_to_half(rgb[c]);
            }
            rgba[(static_cast<usize>(y) * w + x) * 4u + 3u] = 0x3C00u;
        }
    }
    return rgba;
}

f64 psnr(f64 mse, f64 peak) {
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(peak * peak / mse);
}

/// Decode one level with the independent oracle when available, else with the cook's decoder.
/// LDR formats → RGBA8, BC6H → RGBA float.
bool oracle_decode(BcFormat format, const std::vector<u8>& blocks, u32 w, u32 h, std::vector<f32>& out,
                   f64* maxOwnDiff) {
    BcSourceImage own;
    if (!decode_bc_image(format, blocks.data(), blocks.size(), w, h, own)) {
        return false;
    }
    out.assign(static_cast<usize>(w) * h * 4u, 0.f);
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = format == BcFormat::BC6H ? half_to_float(own.rgba16f[i]) : static_cast<f32>(own.rgba8[i]);
    }
    *maxOwnDiff = 0.0;
#if defined(FUSE_COOK_TEST_HAS_BC_ORACLE)
    namespace rl = fuse::relight::mods::assets;
    const u32 bw = (w + 3u) / 4u;
    const u32 bb = bc_block_bytes(format);
    for (u32 by = 0; by < (h + 3u) / 4u; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* blk = blocks.data() + (static_cast<usize>(by) * bw + bx) * bb;
            f32 texel[16][4] = {};
            if (format == BcFormat::BC1 || format == BcFormat::BC7) {
                std::uint8_t t8[16][4];
                if (format == BcFormat::BC1) {
                    rl::decodeBc1Block(blk, t8, false);
                } else {
                    rl::decodeBc7Block(blk, t8);
                }
                for (u32 t = 0; t < 16u; ++t) {
                    for (u32 c = 0; c < 4u; ++c) {
                        texel[t][c] = t8[t][c];
                    }
                }
            } else if (format == BcFormat::BC4 || format == BcFormat::BC5) {
                f32 r[16];
                f32 g[16] = {};
                rl::decodeBc4Block(blk, false, r);
                if (format == BcFormat::BC5) {
                    rl::decodeBc4Block(blk + 8, false, g);
                }
                for (u32 t = 0; t < 16u; ++t) {
                    texel[t][0] = r[t] * 255.f;
                    texel[t][1] = g[t] * 255.f;
                    texel[t][3] = 255.f;
                }
            } else {
                std::uint16_t hf[16][3];
                rl::decodeBc6hBlock(blk, false, hf);
                for (u32 t = 0; t < 16u; ++t) {
                    for (u32 c = 0; c < 3u; ++c) {
                        texel[t][c] = rl::halfToFloat(hf[t][c]);
                    }
                    texel[t][3] = 1.f;
                }
            }
            for (u32 t = 0; t < 16u; ++t) {
                const u32 px = bx * 4u + t % 4u;
                const u32 py = by * 4u + t / 4u;
                if (px >= w || py >= h) {
                    continue;
                }
                for (u32 c = 0; c < 4u; ++c) {
                    f32& dst = out[(static_cast<usize>(py) * w + px) * 4u + c];
                    const f64 diff = std::fabs(static_cast<f64>(dst) - texel[t][c]);
                    const f64 rel = format == BcFormat::BC6H ? diff / std::max(1e-3, std::fabs(static_cast<f64>(texel[t][c]))) : diff;
                    *maxOwnDiff = std::max(*maxOwnDiff, rel);
                    dst = texel[t][c];
                }
            }
        }
    }
#endif
    return true;
}

struct FormatGate {
    BcFormat format;
    f64 min_psnr_db;
    f64 max_own_decoder_diff; ///< cook decoder vs oracle (8-bit units; relative for BC6H)
};

// Per-format PSNR gates (asset plan W0.3). Measured on the 64×64 synthetic image above (oracle
// decode): BC1 37.2 (RGB), BC4 50.8 (R), BC5 51.6 (RG), BC6H 52.7 (RGB, Reinhard tone-mapped),
// BC7 40.0 dB (RGBA). The gates sit 1.5–3 dB below: they catch an encoder regression, not rounding
// noise. BC1 decoders legitimately differ by up to 2 (565 expansion by bit replication vs rounding,
// truncating vs rounding thirds); BC4/BC5 by < 1 (integer vs float interpolation).
constexpr FormatGate kGates[] = {
    {BcFormat::BC1, 35.0, 2.0},
    {BcFormat::BC4, 48.0, 1.0},
    {BcFormat::BC5, 48.0, 1.0},
    {BcFormat::BC6H, 50.0, 0.0},
    {BcFormat::BC7, 38.5, 0.0},
};

f64 measure_psnr(BcFormat format, const BcSourceImage& src, const std::vector<u8>& blocks, f64* ownDiff) {
    std::vector<f32> decoded;
    if (!oracle_decode(format, blocks, src.width, src.height, decoded, ownDiff)) {
        return 0.0;
    }
    f64 sum = 0.0;
    usize count = 0;
    const usize texels = static_cast<usize>(src.width) * src.height;
    for (usize i = 0; i < texels; ++i) {
        u32 channels = 3u;
        if (format == BcFormat::BC4) {
            channels = 1u;
        } else if (format == BcFormat::BC5) {
            channels = 2u;
        } else if (format == BcFormat::BC7) {
            channels = 4u;
        }
        for (u32 c = 0; c < channels; ++c) {
            f64 a = 0.0;
            f64 b = decoded[i * 4u + c];
            if (format == BcFormat::BC6H) {
                a = half_to_float(src.rgba16f[i * 4u + c]);
                // Reinhard tone map to [0, 1] so the metric weighs dark and bright regions alike.
                a = a / (1.0 + a) * 255.0;
                b = b / (1.0 + b) * 255.0;
            } else {
                a = src.rgba8[i * 4u + c];
            }
            sum += (a - b) * (a - b);
            ++count;
        }
    }
    return psnr(sum / static_cast<f64>(count), 255.0);
}

void test_per_format_psnr_gates() {
    BcSourceImage ldr;
    ldr.width = 64;
    ldr.height = 64;
    ldr.rgba8 = make_image(64, 64, 1);
    BcSourceImage hdr;
    hdr.width = 64;
    hdr.height = 64;
    hdr.rgba16f = make_hdr_image(64, 64);
    for (const FormatGate& gate : kGates) {
        const BcSourceImage& src = gate.format == BcFormat::BC6H ? hdr : ldr;
        std::vector<u8> blocks;
        std::string error;
        check(encode_bc_image(gate.format, src, blocks, &error), std::string("encode ") + bc_format_name(gate.format));
        check(blocks.size() == static_cast<usize>(bc_block_count(64, 64)) * bc_block_bytes(gate.format),
              std::string("block bytes ") + bc_format_name(gate.format));
        f64 ownDiff = 0.0;
        const f64 db = measure_psnr(gate.format, src, blocks, &ownDiff);
        std::printf("  %-4s PSNR %.2f dB (gate %.1f), own-vs-oracle max diff %.4f\n", bc_format_name(gate.format), db,
                    gate.min_psnr_db, ownDiff);
        check(db >= gate.min_psnr_db, std::string("PSNR gate ") + bc_format_name(gate.format));
        check(ownDiff <= gate.max_own_decoder_diff + 1e-9,
              std::string("cook decoder agrees with oracle ") + bc_format_name(gate.format));
        // Determinism: identical input → identical blocks.
        std::vector<u8> again;
        check(encode_bc_image(gate.format, src, again, &error) && again == blocks,
              std::string("deterministic ") + bc_format_name(gate.format));
    }
}

void test_edge_cases() {
    // Solid blocks are reproduced (almost) exactly; odd sizes pad by clamping; UF16 clamps negatives.
    for (BcFormat format : {BcFormat::BC1, BcFormat::BC4, BcFormat::BC5, BcFormat::BC7}) {
        BcSourceImage solid;
        solid.width = 5;
        solid.height = 3;
        for (u32 i = 0; i < 15u; ++i) {
            solid.rgba8.insert(solid.rgba8.end(), {200, 100, 50, 255});
        }
        std::vector<u8> blocks;
        check(encode_bc_image(format, solid, blocks), "encode solid");
        check(blocks.size() == 2u * bc_block_bytes(format), "5x3 → 2 blocks");
        BcSourceImage back;
        check(decode_bc_image(format, blocks.data(), blocks.size(), 5, 3, back), "decode solid");
        const u32 channels = format == BcFormat::BC4 ? 1u : format == BcFormat::BC5 ? 2u : 3u;
        for (u32 i = 0; i < 15u; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                const s32 diff = std::abs(static_cast<s32>(back.rgba8[i * 4u + c]) - static_cast<s32>(solid.rgba8[i * 4u + c]));
                check(diff <= (format == BcFormat::BC1 ? 4 : 1), std::string("solid colour ") + bc_format_name(format));
            }
        }
    }
    BcSourceImage hdr;
    hdr.width = 4;
    hdr.height = 4;
    for (u32 i = 0; i < 16u; ++i) {
        const f32 v = i == 0u ? -5.f : (i == 1u ? 1e9f : static_cast<f32>(i));
        hdr.rgba16f.insert(hdr.rgba16f.end(), {float_to_half(v), float_to_half(v * 0.5f), float_to_half(0.25f), 0x3C00u});
    }
    std::vector<u8> blocks;
    check(encode_bc_image(BcFormat::BC6H, hdr, blocks), "encode bc6h extremes");
    BcSourceImage back;
    check(decode_bc_image(BcFormat::BC6H, blocks.data(), blocks.size(), 4, 4, back), "decode bc6h extremes");
    check(half_to_float(back.rgba16f[0]) >= 0.f && half_to_float(back.rgba16f[0]) < 1.f, "negative clamps to ~0");
    check(half_to_float(back.rgba16f[4]) > 30000.f, "+huge clamps to the UF16 max");
    // Wrong source kind is refused.
    BcSourceImage empty;
    empty.width = 4;
    empty.height = 4;
    check(!encode_bc_image(BcFormat::BC7, empty, blocks), "missing pixels refused");
    // Half conversions.
    for (f32 v : {0.f, 1.f, -2.5f, 65504.f, 6.1e-5f, 3.0e-7f, 0.333f}) {
        const f32 back2 = half_to_float(float_to_half(v));
        check(std::fabs(back2 - v) <= std::fabs(v) * 1e-3f + 6e-8f, "half round trip");
    }
}

void test_normal_maps_bc5() {
    TextureSource source;
    source.width = 64;
    source.height = 64;
    source.rgba8 = make_normal_map(64, 64);
    TextureCookOptions options;
    options.normal_map = true; // format and colour space follow from this
    CookedTexture cooked;
    const CookStubWriteResult result = cook_texture_image(source, options, cooked);
    check(result.ok, "normal map cook: " + result.note);
    check(cooked.format == BcFormat::BC5 && cooked.compression == "BC5", "normal map → BC5");
    check(!cooked.srgb && cooked.normal_map, "normal map is linear and flagged");
    check(cooked.levels.size() == 7u, "normal map full mip chain");

    // Decoded normals (Z reconstructed) vs the source unit vectors.
    std::vector<f32> decoded;
    f64 ownDiff = 0.0;
    check(oracle_decode(BcFormat::BC5, cooked.levels[0].blocks, 64, 64, decoded, &ownDiff), "decode normal map");
    f64 sumDeg = 0.0;
    f64 maxDeg = 0.0;
    f64 meanZ = 0.0;
    for (usize i = 0; i < 64u * 64u; ++i) {
        f32 src[3];
        f32 len = 0.f;
        for (u32 d = 0; d < 3u; ++d) {
            src[d] = static_cast<f32>(source.rgba8[i * 4u + d]) / 127.5f - 1.f;
            len += src[d] * src[d];
        }
        len = std::sqrt(len);
        const f32 x = decoded[i * 4u] / 127.5f - 1.f;
        const f32 y = decoded[i * 4u + 1u] / 127.5f - 1.f;
        const f32 z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
        const f32 dot = std::clamp((x * src[0] + y * src[1] + z * src[2]) / len, -1.f, 1.f);
        const f64 deg = std::acos(static_cast<f64>(dot)) * 57.29577951308232;
        sumDeg += deg;
        maxDeg = std::max(maxDeg, deg);
        meanZ += z;
    }
    const f64 meanDeg = sumDeg / (64.0 * 64.0);
    meanZ /= 64.0 * 64.0;
    std::printf("  BC5 normal map: mean angular error %.3f deg, max %.3f deg, mean Z %.3f\n", meanDeg, maxDeg, meanZ);
    // Measured 1.21 / 3.86 deg (includes the 8-bit source quantisation and Z reconstruction).
    check(meanDeg < 1.5, "BC5 normal mean angular error < 1.5 deg");
    check(maxDeg < 5.0, "BC5 normal max angular error < 5 deg");
    check(meanZ > 0.7, "normal map mean Z > 0.7 (asset_normal_map gate)");

    // Mips stay unit length (renormalised, not box-shrunk).
    std::vector<f32> mip;
    check(oracle_decode(BcFormat::BC5, cooked.levels[3].blocks, 8, 8, mip, &ownDiff), "decode normal mip");
    for (usize i = 0; i < 64u; ++i) {
        const f32 x = mip[i * 4u] / 127.5f - 1.f;
        const f32 y = mip[i * 4u + 1u] / 127.5f - 1.f;
        check(x * x + y * y <= 1.02f, "normal mip xy inside the unit disc");
    }

    // Container: header records BC5, linear, GL convention; the file parses back identically.
    const std::vector<u8> bytes = serialize_cooked_texture(cooked);
    const std::string text(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min<usize>(bytes.size(), 400u)));
    check(text.find("compression=BC5\n") != std::string::npos, "header compression=BC5");
    check(text.find("srgb=0\n") != std::string::npos, "header srgb=0");
    check(text.find("normal_convention=gl\n") != std::string::npos, "header normal_convention=gl");
    CookedTexture parsed;
    std::string error;
    check(parse_cooked_texture(bytes.data(), bytes.size(), parsed, &error), "parse BC5: " + error);
    check(parsed.normal_map && parsed.format == BcFormat::BC5 && serialize_cooked_texture(parsed) == bytes,
          "BC5 container round trip");

    // Requesting BC7 for a normal map still yields BC5 (never BC7/BC1 normals).
    options.format = BcFormat::BC7;
    CookedTexture forced;
    check(cook_texture_image(source, options, forced).ok && forced.format == BcFormat::BC5, "normal map forced to BC5");
}

void test_arrays_and_cubes() {
    TextureSource source;
    source.width = 32;
    source.height = 32;
    source.layers = 3;
    for (u32 layer = 0; layer < 3u; ++layer) {
        const std::vector<u8> img = make_image(32, 32, 10u + layer);
        source.rgba8.insert(source.rgba8.end(), img.begin(), img.end());
    }
    for (BcFormat format : {BcFormat::BC1, BcFormat::BC4, BcFormat::BC7}) {
        TextureCookOptions options;
        options.format = format;
        CookedTexture cooked;
        check(cook_texture_image(source, options, cooked).ok, "array cook");
        check(cooked.layers == 3u && cooked.levels.size() == 6u, "array layers and mips");
        for (const CookedTexture::Level& level : cooked.levels) {
            check(level.blocks.size() == static_cast<usize>(bc_block_count(level.width, level.height)) * 3u *
                                             bc_block_bytes(format),
                  "array level size");
        }
        // Each layer decodes to its own image.
        const usize layerBytes = static_cast<usize>(bc_block_count(32, 32)) * bc_block_bytes(format);
        for (u32 layer = 0; layer < 3u; ++layer) {
            BcSourceImage src;
            src.width = 32;
            src.height = 32;
            src.rgba8.assign(source.rgba8.begin() + static_cast<std::ptrdiff_t>(32u * 32u * 4u * layer),
                             source.rgba8.begin() + static_cast<std::ptrdiff_t>(32u * 32u * 4u * (layer + 1u)));
            const std::vector<u8> blocks(cooked.levels[0].blocks.begin() + static_cast<std::ptrdiff_t>(layerBytes * layer),
                                         cooked.levels[0].blocks.begin() + static_cast<std::ptrdiff_t>(layerBytes * (layer + 1u)));
            f64 ownDiff = 0.0;
            const f64 db = measure_psnr(format, src, blocks, &ownDiff);
            check(db > 30.0, std::string("array layer decodes to its own content ") + bc_format_name(format));
        }
        const std::vector<u8> bytes = serialize_cooked_texture(cooked);
        CookedTexture parsed;
        std::string error;
        check(parse_cooked_texture(bytes.data(), bytes.size(), parsed, &error) && parsed.layers == 3u,
              "array parse: " + error);
        check(serialize_cooked_texture(parsed) == bytes, "array container round trip");
    }

    // Cube map: 6 square faces.
    TextureSource cube;
    cube.width = 16;
    cube.height = 16;
    cube.layers = 6;
    for (u32 face = 0; face < 6u; ++face) {
        const std::vector<u8> img = make_image(16, 16, 20u + face);
        cube.rgba8.insert(cube.rgba8.end(), img.begin(), img.end());
    }
    TextureCookOptions cubeOptions;
    cubeOptions.cube = true;
    CookedTexture cooked;
    check(cook_texture_image(cube, cubeOptions, cooked).ok && cooked.cube && cooked.layers == 6u, "cube cook");
    const std::vector<u8> bytes = serialize_cooked_texture(cooked);
    CookedTexture parsed;
    check(parse_cooked_texture(bytes.data(), bytes.size(), parsed) && parsed.cube, "cube parse");
    cube.layers = 5;
    cube.rgba8.resize(16u * 16u * 4u * 5u);
    check(!cook_texture_image(cube, cubeOptions, cooked).ok, "cube with 5 faces refused");
    TextureSource tooMany;
    tooMany.width = 1;
    tooMany.height = 1;
    tooMany.layers = kMaxCookTextureLayers + 1u;
    tooMany.rgba8.assign(static_cast<usize>(tooMany.layers) * 4u, 0u);
    check(cook_texture_image(tooMany, TextureCookOptions{}, cooked).failure == CookFailure::InvalidImageDimensions,
          "too many layers refused");
}

void test_hdr_bc6h_and_srgb() {
    TextureSource hdr;
    hdr.width = 32;
    hdr.height = 16;
    hdr.rgba16f = make_hdr_image(32, 16);
    TextureCookOptions options;
    options.format = BcFormat::BC6H;
    options.srgb = true; // ignored for BC6H
    options.texel_m = 0.25f;
    CookedTexture cooked;
    check(cook_texture_image(hdr, options, cooked).ok, "BC6H cook");
    check(!cooked.srgb && cooked.levels.size() == 6u && cooked.texel_m == 0.25f, "BC6H linear, mips, texel_m");
    const std::vector<u8> bytes = serialize_cooked_texture(cooked);
    CookedTexture parsed;
    check(parse_cooked_texture(bytes.data(), bytes.size(), parsed) && parsed.texel_m == 0.25f &&
              parsed.format == BcFormat::BC6H,
          "BC6H parse");
    // HDR pixels cannot go to an LDR format.
    options.format = BcFormat::BC7;
    check(!cook_texture_image(hdr, options, cooked).ok, "HDR → BC7 refused");

    // BC1 keeps the sRGB flag; BC4 is always linear.
    TextureSource ldr;
    ldr.width = 8;
    ldr.height = 8;
    ldr.rgba8 = make_image(8, 8, 3);
    TextureCookOptions bc1;
    bc1.format = BcFormat::BC1;
    check(cook_texture_image(ldr, bc1, cooked).ok && cooked.srgb, "BC1 sRGB");
    TextureCookOptions bc4;
    bc4.format = BcFormat::BC4;
    check(cook_texture_image(ldr, bc4, cooked).ok && !cooked.srgb, "BC4 linear");
}

/// Minimal uncompressed 32-bit TGA (top-left origin) so the strict file cook can be driven end to end.
void write_tga(const std::string& path, u32 w, u32 h, const std::vector<u8>& rgba) {
    std::vector<u8> out(18, 0);
    out[2] = 2;
    out[12] = static_cast<u8>(w & 0xFFu);
    out[13] = static_cast<u8>(w >> 8);
    out[14] = static_cast<u8>(h & 0xFFu);
    out[15] = static_cast<u8>(h >> 8);
    out[16] = 32;
    out[17] = 0x28; // 8 alpha bits, top-left origin
    for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
        out.insert(out.end(), {rgba[i * 4u + 2u], rgba[i * 4u + 1u], rgba[i * 4u], rgba[i * 4u + 3u]});
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                                static_cast<std::streamsize>(out.size()));
}

void test_file_cook_and_legacy_container() {
    const std::vector<u8> img = make_image(20, 12, 5);
    const std::string tga = temp_path("src.tga");
    write_tga(tga, 20, 12, img);

    // Default options: byte-identical to the original BC7 writer (version-1 container).
    const std::string viaNew = temp_path("new.fusetex");
    const std::string viaOld = temp_path("old.fusetex");
    check(cook_texture_file(tga, viaNew, TextureCookOptions{}).ok, "default cook");
    check(write_texture_bc7_rgba(img.data(), 20, 12, viaOld, true).ok, "legacy writer");
    check(read_file(viaNew) == read_file(viaOld), "default BC7 cook keeps the v1 container byte for byte");
    check(cook_texture_bc7_file(tga, temp_path("old2.fusetex"), true).ok &&
              read_file(temp_path("old2.fusetex")) == read_file(viaOld),
          "cook_texture_bc7_file unchanged");
    CookedTexture legacy;
    std::string error;
    check(load_cooked_texture(viaOld, legacy, &error), "v1 container loads: " + error);
    check(legacy.format == BcFormat::BC7 && legacy.srgb && legacy.layers == 1u && legacy.levels.size() == 5u,
          "v1 container fields");

    // Every format through the file path, then back through the loader.
    for (BcFormat format : {BcFormat::BC1, BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7}) {
        TextureCookOptions options;
        options.format = format;
        options.srgb = false;
        const std::string out = temp_path(std::string("fmt_") + bc_format_name(format) + ".fusetex");
        const CookStubWriteResult result = cook_texture_file(tga, out, options);
        check(result.ok, std::string("file cook ") + bc_format_name(format) + ": " + result.note);
        CookedTexture loaded;
        check(load_cooked_texture(out, loaded, &error) && loaded.format == format && loaded.levels.size() == 5u,
              std::string("file cook loads ") + bc_format_name(format) + ": " + error);
        const std::vector<u8> first = read_file(out);
        check(cook_texture_file(tga, out, options).ok && read_file(out) == first,
              std::string("file cook deterministic ") + bc_format_name(format));
    }

    // Corrupt containers are refused.
    std::vector<u8> bytes = read_file(temp_path("fmt_BC4.fusetex"));
    std::vector<u8> truncated(bytes.begin(), bytes.end() - 3);
    CookedTexture bad;
    check(!parse_cooked_texture(truncated.data(), truncated.size(), bad), "truncated container refused");
    std::string text(bytes.begin(), bytes.end());
    const std::size_t at = text.find("block_bytes=8");
    check(at != std::string::npos, "BC4 header has block_bytes=8");
    text.replace(at, 13, "block_bytes=9");
    check(!parse_cooked_texture(reinterpret_cast<const u8*>(text.data()), text.size(), bad), "wrong block_bytes refused");
    // Missing / corrupt sources fail with the strict failure classes.
    check(cook_texture_file(temp_path("missing.tga"), temp_path("x.fusetex"), TextureCookOptions{}).failure ==
              CookFailure::CorruptImage,
          "missing source → CorruptImage");
}

} // namespace

int main() {
    test_per_format_psnr_gates();
    test_edge_cases();
    test_normal_maps_bc5();
    test_arrays_and_cubes();
    test_hdr_bc6h_and_srgb();
    test_file_cook_and_legacy_container();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_texture_bcn: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_asset_texture_bcn: all checks passed\n");
    return EXIT_SUCCESS;
}
