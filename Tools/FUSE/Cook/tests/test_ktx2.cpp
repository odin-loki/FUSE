// Asset plan W0.4 gate (docs/plans/FUSE_ASSET_PLAN.md §1.3, §6 Wave 0): KTX2 import/export as a
// transport format. Round trips: writer ↔ reader for every supported vkFormat (2D, arrays, cube maps,
// mip chains); cooked `.fusetex` → KTX2 → `.fusetex` byte for byte; uncompressed KTX2 as a cook source
// (RGBA8, RGBA16F, RGBA32F); `fuse_cook --texture` with `.ktx2` input and output; malformed files and
// unsupported features (supercompression, Basis/UASTC, 3D) refused.
#include <fuse/cook/bcn_encoder.hpp>
#include <fuse/cook/ktx2.hpp>
#include <fuse/cook/texture_cook.hpp>

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

std::filesystem::path temp_dir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_asset_ktx2";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string temp_path(const std::string& name) {
    return (temp_dir() / name).string();
}

std::vector<u8> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
}

u32 le32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) |
           (static_cast<u32>(b[at + 3]) << 24);
}

u64 le64(const std::vector<u8>& b, usize at) {
    return static_cast<u64>(le32(b, at)) | (static_cast<u64>(le32(b, at + 4)) << 32);
}

std::vector<u8> make_rgba(u32 w, u32 h, u32 layers, u32 seed) {
    std::vector<u8> rgba(static_cast<usize>(w) * h * layers * 4u);
    u32 state = seed * 747796405u + 2891336453u;
    for (usize i = 0; i < rgba.size(); i += 4u) {
        state = state * 1664525u + 1013904223u;
        const usize texel = i / 4u;
        const u32 x = static_cast<u32>(texel % w);
        const u32 y = static_cast<u32>((texel / w) % h);
        rgba[i + 0] = static_cast<u8>((x * 255u) / std::max(1u, w - 1u));
        rgba[i + 1] = static_cast<u8>((y * 255u) / std::max(1u, h - 1u));
        rgba[i + 2] = static_cast<u8>(128u + ((state >> 28) & 7u) + 20u * static_cast<u32>(texel / (static_cast<usize>(w) * h)));
        rgba[i + 3] = 255u;
    }
    return rgba;
}

/// Structural checks against the KTX 2.0 specification layout.
void check_ktx2_layout(const std::vector<u8>& b, u32 vkFormat, u32 levels, const std::string& what) {
    static const u8 kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    check(b.size() > 80u && std::memcmp(b.data(), kId, 12) == 0, what + ": identifier");
    check(le32(b, 12) == vkFormat, what + ": vkFormat");
    check(le32(b, 40) == levels, what + ": levelCount");
    check(le32(b, 44) == 0u, what + ": supercompressionScheme none");
    const u32 dfdOffset = le32(b, 48);
    const u32 dfdLength = le32(b, 52);
    check(dfdOffset == 80u + 24u * levels, what + ": DFD follows the level index");
    check(le32(b, dfdOffset) == dfdLength, what + ": dfdTotalSize");
    check(le32(b, dfdOffset + 4u) == 0u, what + ": Khronos basic descriptor");
    check((le32(b, dfdOffset + 8u) & 0xFFFFu) == 2u, what + ": KDF version 2");
    check(le64(b, 72) == 0u, what + ": no supercompression global data");
    const u32 kvdOffset = le32(b, 56);
    check(kvdOffset == dfdOffset + dfdLength && kvdOffset % 4u == 0u, what + ": KVD follows the DFD");
    // Levels are stored smallest first and each offset is aligned.
    u64 previousOffset = ~0ull;
    for (u32 level = 0; level < levels; ++level) {
        const u64 offset = le64(b, 80u + 24u * level);
        const u64 length = le64(b, 80u + 24u * level + 8u);
        check(offset % 4u == 0u && offset + length <= b.size(), what + ": level bounds/alignment");
        check(level == 0u || offset < previousOffset, what + ": smaller levels stored earlier");
        previousOffset = offset;
    }
}

// Reference file written by Khronos `ktx create` v4.3.2 (KTX-Software, Apache-2.0):
//   ktx create --format R8G8B8A8_SRGB --generate-mipmap ref.png ref_rgba8.ktx2
// where ref.png is 8×8 RGBA8 with texel (x, y) = (32x, 32y, 4xy mod 256, 255). 660 bytes,
// sha256 9b13eaa8ff08fdd3793c6ce9758ab50669902f121995b667ec467053f65edee2. It checks the reader
// against an independent writer, and the writer's layout (header, level index, DFD, KVD, level
// placement) against the reference byte for byte.
const u8 kKtxCreateReference[] = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a, 0x2b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x00,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x94, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x58, 0x00, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x18, 0x00, 0x07, 0x1f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x4b, 0x54, 0x58, 0x77, 0x72, 0x69, 0x74, 0x65,
    0x72, 0x00, 0x6b, 0x74, 0x78, 0x20, 0x63, 0x72, 0x65, 0x61, 0x74, 0x65, 0x20, 0x76, 0x34, 0x2e, 0x33, 0x2e, 0x32, 0x7e,
    0x36, 0x20, 0x2f, 0x20, 0x6c, 0x69, 0x62, 0x6b, 0x74, 0x78, 0x20, 0x76, 0x34, 0x2e, 0x33, 0x2e, 0x32, 0x7e, 0x31, 0x00,
    0x70, 0x70, 0x31, 0xff, 0x46, 0x46, 0x13, 0xff, 0x9a, 0x46, 0x2a, 0xff, 0x46, 0x9a, 0x2a, 0xff, 0x9a, 0x9a, 0x5d, 0xff,
    0x1e, 0x1e, 0x04, 0xff, 0x4b, 0x1e, 0x09, 0xff, 0x95, 0x1e, 0x12, 0xff, 0xc2, 0x1e, 0x17, 0xff, 0x1e, 0x4b, 0x09, 0xff,
    0x4b, 0x4b, 0x16, 0xff, 0x95, 0x4b, 0x2c, 0xff, 0xc2, 0x4b, 0x39, 0xff, 0x1e, 0x95, 0x12, 0xff, 0x4b, 0x95, 0x2c, 0xff,
    0x95, 0x95, 0x56, 0xff, 0xc2, 0x95, 0x71, 0xff, 0x1e, 0xc2, 0x17, 0xff, 0x4b, 0xc2, 0x39, 0xff, 0x95, 0xc2, 0x71, 0xff,
    0xc2, 0xc2, 0x93, 0xff, 0x00, 0x00, 0x00, 0xff, 0x20, 0x00, 0x00, 0xff, 0x40, 0x00, 0x00, 0xff, 0x60, 0x00, 0x00, 0xff,
    0x80, 0x00, 0x00, 0xff, 0xa0, 0x00, 0x00, 0xff, 0xc0, 0x00, 0x00, 0xff, 0xe0, 0x00, 0x00, 0xff, 0x00, 0x20, 0x00, 0xff,
    0x20, 0x20, 0x04, 0xff, 0x40, 0x20, 0x08, 0xff, 0x60, 0x20, 0x0c, 0xff, 0x80, 0x20, 0x10, 0xff, 0xa0, 0x20, 0x14, 0xff,
    0xc0, 0x20, 0x18, 0xff, 0xe0, 0x20, 0x1c, 0xff, 0x00, 0x40, 0x00, 0xff, 0x20, 0x40, 0x08, 0xff, 0x40, 0x40, 0x10, 0xff,
    0x60, 0x40, 0x18, 0xff, 0x80, 0x40, 0x20, 0xff, 0xa0, 0x40, 0x28, 0xff, 0xc0, 0x40, 0x30, 0xff, 0xe0, 0x40, 0x38, 0xff,
    0x00, 0x60, 0x00, 0xff, 0x20, 0x60, 0x0c, 0xff, 0x40, 0x60, 0x18, 0xff, 0x60, 0x60, 0x24, 0xff, 0x80, 0x60, 0x30, 0xff,
    0xa0, 0x60, 0x3c, 0xff, 0xc0, 0x60, 0x48, 0xff, 0xe0, 0x60, 0x54, 0xff, 0x00, 0x80, 0x00, 0xff, 0x20, 0x80, 0x10, 0xff,
    0x40, 0x80, 0x20, 0xff, 0x60, 0x80, 0x30, 0xff, 0x80, 0x80, 0x40, 0xff, 0xa0, 0x80, 0x50, 0xff, 0xc0, 0x80, 0x60, 0xff,
    0xe0, 0x80, 0x70, 0xff, 0x00, 0xa0, 0x00, 0xff, 0x20, 0xa0, 0x14, 0xff, 0x40, 0xa0, 0x28, 0xff, 0x60, 0xa0, 0x3c, 0xff,
    0x80, 0xa0, 0x50, 0xff, 0xa0, 0xa0, 0x64, 0xff, 0xc0, 0xa0, 0x78, 0xff, 0xe0, 0xa0, 0x8c, 0xff, 0x00, 0xc0, 0x00, 0xff,
    0x20, 0xc0, 0x18, 0xff, 0x40, 0xc0, 0x30, 0xff, 0x60, 0xc0, 0x48, 0xff, 0x80, 0xc0, 0x60, 0xff, 0xa0, 0xc0, 0x78, 0xff,
    0xc0, 0xc0, 0x90, 0xff, 0xe0, 0xc0, 0xa8, 0xff, 0x00, 0xe0, 0x00, 0xff, 0x20, 0xe0, 0x1c, 0xff, 0x40, 0xe0, 0x38, 0xff,
    0x60, 0xe0, 0x54, 0xff, 0x80, 0xe0, 0x70, 0xff, 0xa0, 0xe0, 0x8c, 0xff, 0xc0, 0xe0, 0xa8, 0xff, 0xe0, 0xe0, 0xc4, 0xff,
};

void test_ktx_create_reference() {
    const std::vector<u8> reference(std::begin(kKtxCreateReference), std::end(kKtxCreateReference));
    check(reference.size() == 660u, "reference fixture size");
    Ktx2Image image;
    std::string error;
    check(read_ktx2(reference.data(), reference.size(), image, &error), "reads ktx create output: " + error);
    check(image.vk_format == vk_format::kR8G8B8A8Srgb && image.width == 8u && image.height == 8u &&
              image.layer_count == 0u && image.face_count == 1u && image.levels.size() == 4u,
          "reference header fields");
    bool pixels = image.levels.size() == 4u && image.levels[0].size() == 256u;
    for (u32 y = 0; pixels && y < 8u; ++y) {
        for (u32 x = 0; x < 8u; ++x) {
            const u8* t = &image.levels[0][(y * 8u + x) * 4u];
            pixels = pixels && t[0] == x * 32u && t[1] == y * 32u && t[2] == ((x * y * 4u) & 0xFFu) && t[3] == 255u;
        }
    }
    check(pixels, "reference level 0 texels");
    bool writer = false;
    for (const auto& [key, value] : image.key_values) {
        writer = writer || (key == "KTXwriter" && value.rfind("ktx create", 0) == 0);
    }
    check(writer, "reference KTXwriter key");
    std::vector<u8> rewritten;
    check(write_ktx2(image, rewritten, &error), "re-write reference: " + error);
    check(rewritten == reference, "FUSE writer reproduces the ktx create file byte for byte");
    // As a cook source: level 0 → BC7 (sRGB).
    TextureSource source;
    check(ktx2_to_texture_source(image, source, &error) && source.rgba8 == image.levels[0], "reference as a source");
}

void test_writer_reader_round_trip() {
    struct Case {
        u32 vk;
        BcFormat bc;
        bool compressed;
    };
    const Case cases[] = {
        {vk_format::kBc1RgbUnorm, BcFormat::BC1, true},  {vk_format::kBc1RgbSrgb, BcFormat::BC1, true},
        {vk_format::kBc4Unorm, BcFormat::BC4, true},     {vk_format::kBc5Unorm, BcFormat::BC5, true},
        {vk_format::kBc6hUfloat, BcFormat::BC6H, true},  {vk_format::kBc7Unorm, BcFormat::BC7, true},
        {vk_format::kBc7Srgb, BcFormat::BC7, true},      {vk_format::kR8G8B8A8Unorm, BcFormat::BC7, false},
        {vk_format::kR8G8B8A8Srgb, BcFormat::BC7, false}, {vk_format::kR16G16B16A16Sfloat, BcFormat::BC7, false},
        {vk_format::kR32G32B32A32Sfloat, BcFormat::BC7, false},
    };
    struct Shape {
        u32 w;
        u32 h;
        u32 layers;
        u32 faces;
        u32 levels;
    };
    const Shape shapes[] = {{13, 7, 0, 1, 4}, {8, 8, 3, 1, 1}, {8, 8, 0, 6, 4}, {16, 16, 2, 6, 2}};
    for (const Case& c : cases) {
        for (const Shape& shape : shapes) {
            Ktx2Image image;
            image.vk_format = c.vk;
            image.width = shape.w;
            image.height = shape.h;
            image.layer_count = shape.layers;
            image.face_count = shape.faces;
            image.key_values.emplace_back("fuse.test", "value");
            u32 w = shape.w;
            u32 h = shape.h;
            for (u32 level = 0; level < shape.levels; ++level) {
                std::vector<u8> data(ktx2_level_bytes(c.vk, w, h, std::max(1u, shape.layers) * shape.faces));
                for (usize i = 0; i < data.size(); ++i) {
                    data[i] = static_cast<u8>((i * 131u + level * 7u + c.vk) & 0xFFu);
                }
                image.levels.push_back(std::move(data));
                w = std::max(1u, w / 2u);
                h = std::max(1u, h / 2u);
            }
            std::vector<u8> bytes;
            std::string error;
            const std::string what = "vk " + std::to_string(c.vk) + " " + std::to_string(shape.w) + "x" +
                                     std::to_string(shape.h) + " L" + std::to_string(shape.layers) + " F" +
                                     std::to_string(shape.faces);
            check(write_ktx2(image, bytes, &error), what + ": write " + error);
            check_ktx2_layout(bytes, c.vk, shape.levels, what);
            Ktx2Image back;
            check(read_ktx2(bytes.data(), bytes.size(), back, &error), what + ": read " + error);
            check(back.vk_format == c.vk && back.width == shape.w && back.height == shape.h &&
                      back.layer_count == shape.layers && back.face_count == shape.faces && back.levels == image.levels,
                  what + ": fields and level data round trip");
            bool hasWriter = false;
            bool hasTest = false;
            for (const auto& [key, value] : back.key_values) {
                hasWriter = hasWriter || (key == "KTXwriter" && !value.empty());
                hasTest = hasTest || (key == "fuse.test" && value == "value");
            }
            check(hasWriter && hasTest, what + ": key/value data");
            std::vector<u8> again;
            check(write_ktx2(back, again) && again == bytes, what + ": re-write identical");
            check(ktx2_format_is_block_compressed(c.vk) == c.compressed, what + ": compressed flag");
        }
    }
}

void test_cooked_texture_round_trip() {
    // .fusetex → .ktx2 → .fusetex: blocks and metadata survive byte for byte, for every format.
    for (BcFormat format : {BcFormat::BC1, BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7}) {
        TextureSource source;
        source.width = 24;
        source.height = 12;
        source.layers = 2;
        source.rgba8 = make_rgba(24, 12, 2, 7);
        TextureCookOptions options;
        options.format = format;
        options.normal_map = format == BcFormat::BC5;
        options.texel_m = 0.125f;
        CookedTexture cooked;
        check(cook_texture_image(source, options, cooked).ok, std::string("cook ") + bc_format_name(format));
        const std::string name = bc_format_name(format);
        const std::string fusetex = temp_path("rt_" + name + ".fusetex");
        const std::string ktx = temp_path("rt_" + name + ".ktx2");
        const std::string back = temp_path("rt_" + name + "_back.fusetex");
        check(write_cooked_texture(cooked, fusetex).ok, "write fusetex " + name);
        const CookStubWriteResult exported = cook_texture_file(fusetex, ktx, options);
        check(exported.ok, "export ktx2 " + name + ": " + exported.note);
        const CookStubWriteResult imported = cook_texture_file(ktx, back, options);
        check(imported.ok, "import ktx2 " + name + ": " + imported.note);
        check(!read_file(fusetex).empty() && read_file(fusetex) == read_file(back),
              "fusetex → ktx2 → fusetex byte identical " + name);
        CookedTexture viaKtx;
        std::string error;
        check(load_cooked_texture(ktx, viaKtx, &error), "load_cooked_texture(.ktx2) " + name + ": " + error);
        check(viaKtx.levels.size() == cooked.levels.size() && viaKtx.layers == 2u && viaKtx.texel_m == 0.125f &&
                  viaKtx.normal_map == options.normal_map,
              "ktx2 metadata " + name);
        // Transport never transcodes: a BC7 KTX2 cannot be imported as BC1.
        TextureCookOptions other = options;
        other.normal_map = false;
        other.format = format == BcFormat::BC7 ? BcFormat::BC1 : BcFormat::BC7;
        check(!cook_texture_file(ktx, temp_path("rt_mismatch.fusetex"), other).ok, "no transcoding " + name);
    }
}

void test_uncompressed_sources() {
    // RGBA8 KTX2 array → BC7 equals cooking the same pixels in memory.
    TextureSource source;
    source.width = 20;
    source.height = 20;
    source.layers = 3;
    source.rgba8 = make_rgba(20, 20, 3, 11);
    Ktx2Image image;
    std::string error;
    check(texture_source_to_ktx2(source, true, false, image, &error), "rgba8 → ktx2: " + error);
    std::vector<u8> bytes;
    check(write_ktx2(image, bytes, &error), "write rgba8 ktx2");
    check_ktx2_layout(bytes, vk_format::kR8G8B8A8Srgb, 1u, "rgba8 source");
    const std::string ktxPath = temp_path("src_rgba8.ktx2");
    write_file(ktxPath, bytes);
    for (BcFormat format : {BcFormat::BC1, BcFormat::BC7}) {
        TextureCookOptions options;
        options.format = format;
        const std::string out = temp_path(std::string("src_rgba8_") + bc_format_name(format) + ".fusetex");
        check(cook_texture_file(ktxPath, out, options).ok, "cook from rgba8 ktx2");
        CookedTexture direct;
        check(cook_texture_image(source, options, direct).ok, "cook in memory");
        check(read_file(out) == serialize_cooked_texture(direct), "ktx2 source cook == in-memory cook");
    }
    // A single-layer RGBA8 KTX2 with default options takes the v1 BC7 path, like a PNG would.
    TextureSource single;
    single.width = 9;
    single.height = 5;
    single.rgba8 = make_rgba(9, 5, 1, 3);
    check(texture_source_to_ktx2(single, true, false, image) && write_ktx2(image, bytes), "single rgba8 ktx2");
    write_file(temp_path("single.ktx2"), bytes);
    check(cook_texture_file(temp_path("single.ktx2"), temp_path("single.fusetex"), TextureCookOptions{}).ok &&
              write_texture_bc7_rgba(single.rgba8.data(), 9, 5, temp_path("single_ref.fusetex"), true).ok &&
              read_file(temp_path("single.fusetex")) == read_file(temp_path("single_ref.fusetex")),
          "single-layer RGBA8 KTX2 cooks like the v1 BC7 path");

    // RGBA16F and RGBA32F sources → BC6H; KTX2 output directly.
    TextureSource hdr;
    hdr.width = 16;
    hdr.height = 8;
    for (u32 i = 0; i < 16u * 8u; ++i) {
        const f32 v = 0.1f + static_cast<f32>(i) * 0.37f;
        hdr.rgba16f.insert(hdr.rgba16f.end(), {float_to_half(v), float_to_half(v * 0.5f), float_to_half(2.f), 0x3C00u});
    }
    check(texture_source_to_ktx2(hdr, false, false, image) && write_ktx2(image, bytes), "rgba16f ktx2");
    write_file(temp_path("hdr16.ktx2"), bytes);
    Ktx2Image f32Image = image;
    f32Image.vk_format = vk_format::kR32G32B32A32Sfloat;
    f32Image.levels[0].clear();
    for (u16 h : hdr.rgba16f) {
        const f32 v = half_to_float(h);
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        for (u32 s = 0; s < 32u; s += 8u) {
            f32Image.levels[0].push_back(static_cast<u8>((bits >> s) & 0xFFu));
        }
    }
    check(write_ktx2(f32Image, bytes), "rgba32f ktx2");
    write_file(temp_path("hdr32.ktx2"), bytes);
    TextureCookOptions bc6;
    bc6.format = BcFormat::BC6H;
    check(cook_texture_file(temp_path("hdr16.ktx2"), temp_path("hdr16.ktx2.out.ktx2"), bc6).ok, "16F → BC6H ktx2");
    check(cook_texture_file(temp_path("hdr32.ktx2"), temp_path("hdr32.fusetex"), bc6).ok, "32F → BC6H fusetex");
    check(cook_texture_file(temp_path("hdr16.ktx2"), temp_path("hdr16.fusetex"), bc6).ok, "16F → BC6H fusetex");
    check(read_file(temp_path("hdr16.fusetex")) == read_file(temp_path("hdr32.fusetex")),
          "16F and 32F sources of the same values cook identically");
    Ktx2Image out;
    check(read_ktx2_file(temp_path("hdr16.ktx2.out.ktx2"), out, &error) && out.vk_format == vk_format::kBc6hUfloat &&
              out.levels.size() == 5u,
          "BC6H KTX2 export: " + error);
    // HDR KTX2 cannot be cooked to an LDR format.
    check(!cook_texture_file(temp_path("hdr16.ktx2"), temp_path("hdr_bad.fusetex"), TextureCookOptions{}).ok,
          "HDR ktx2 → BC7 refused");
}

void test_rejections() {
    TextureSource source;
    source.width = 8;
    source.height = 8;
    source.rgba8 = make_rgba(8, 8, 1, 1);
    CookedTexture cooked;
    TextureCookOptions options;
    options.format = BcFormat::BC7;
    options.srgb = false;
    check(cook_texture_image(source, options, cooked).ok, "cook for rejection tests");
    Ktx2Image image;
    std::vector<u8> good;
    check(cooked_texture_to_ktx2(cooked, image) && write_ktx2(image, good), "export for rejection tests");
    Ktx2Image parsed;
    std::string error;
    auto refused = [&](std::vector<u8> bytes, const std::string& what) {
        check(!read_ktx2(bytes.data(), bytes.size(), parsed, &error), what);
    };
    auto patch32 = [](std::vector<u8> bytes, usize at, u32 value) {
        for (u32 i = 0; i < 4u; ++i) {
            bytes[at + i] = static_cast<u8>((value >> (i * 8u)) & 0xFFu);
        }
        return bytes;
    };
    check(read_ktx2(good.data(), good.size(), parsed, &error), "good file reads: " + error);
    std::vector<u8> badId = good;
    badId[1] = 'X';
    refused(badId, "bad identifier refused");
    refused(std::vector<u8>(good.begin(), good.begin() + 60), "truncated header refused");
    refused(std::vector<u8>(good.begin(), good.end() - 1), "truncated level data refused");
    refused(patch32(good, 12, 0u), "vkFormat UNDEFINED (Basis/UASTC) refused");
    refused(patch32(good, 12, 147u), "unsupported vkFormat (ETC2) refused");
    refused(patch32(good, 44, 2u), "Zstandard supercompression refused");
    refused(patch32(good, 28, 4u), "3D texture refused");
    refused(patch32(good, 36, 3u), "faceCount 3 refused");
    refused(patch32(good, 40, 9u), "levelCount beyond the mip chain refused");
    refused(patch32(good, 16, 2u), "typeSize mismatch refused");
    refused(patch32(good, 12, vk_format::kBc1RgbUnorm), "DFD model mismatch refused");
    refused(patch32(good, 12, vk_format::kBc7Srgb), "DFD transfer mismatch refused");
    refused(patch32(good, 80 + 8, 17u), "level length mismatch refused");
    refused(patch32(good, 80, 3u), "misaligned level refused");
    // An uncompressed KTX2 is not a cooked texture; a BCn one is not a pixel source.
    TextureSource px;
    check(!ktx2_to_texture_source(image, px), "BCn KTX2 is not a pixel source");
    Ktx2Image raw;
    check(texture_source_to_ktx2(source, false, false, raw), "raw ktx2");
    CookedTexture notCooked;
    check(!ktx2_to_cooked_texture(raw, notCooked), "uncompressed KTX2 is not a cooked texture");
    const std::string garbage = temp_path("garbage.ktx2");
    write_file(garbage, std::vector<u8>(100, 7u));
    check(cook_texture_file(garbage, temp_path("garbage.fusetex"), TextureCookOptions{}).failure ==
              CookFailure::CorruptImage,
          "garbage ktx2 → CorruptImage");
}

void test_fuse_cook_cli(const char* fuseCook) {
    if (fuseCook == nullptr) {
        std::printf("  fuse_cook CLI check skipped (no executable path given)\n");
        return;
    }
    // Uncompressed RGBA8 KTX2 source.
    TextureSource source;
    source.width = 16;
    source.height = 16;
    source.rgba8 = make_rgba(16, 16, 1, 5);
    Ktx2Image image;
    std::vector<u8> bytes;
    check(texture_source_to_ktx2(source, true, false, image) && write_ktx2(image, bytes), "cli source");
    const std::filesystem::path dir = temp_dir() / "cli";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_file((dir / "in.ktx2").string(), bytes);
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(dir); // fuse_cook keeps its cook cache in the working directory
    auto run = [&](const std::string& args) {
        const std::string command = "\"" + std::string(fuseCook) + "\" " + args;
        return std::system(command.c_str());
    };
    check(run("--texture --input in.ktx2 --output out.fusetex") == 0, "fuse_cook --texture .ktx2 → .fusetex");
    check(run("--texture --format BC1 --input in.ktx2 --output out_bc1.ktx2") == 0, "fuse_cook --texture → .ktx2");
    check(run("--texture --format BC1 --input out_bc1.ktx2 --output out_bc1.fusetex") == 0,
          "fuse_cook --texture BC1 .ktx2 → .fusetex");
    check(run("--texture --normal-map --input in.ktx2 --output normal.ktx2") == 0, "fuse_cook --normal-map → .ktx2");
    check(run("--texture --format BC7 --input out_bc1.ktx2 --output bad.fusetex") != 0,
          "fuse_cook refuses transcoding BC1 → BC7");
    std::filesystem::current_path(previous);

    CookedTexture cooked;
    std::string error;
    check(load_cooked_texture((dir / "out.fusetex").string(), cooked, &error) && cooked.format == BcFormat::BC7,
          "cli BC7 output loads: " + error);
    Ktx2Image bc1;
    check(read_ktx2_file((dir / "out_bc1.ktx2").string(), bc1, &error) && bc1.vk_format == vk_format::kBc1RgbSrgb,
          "cli BC1 KTX2 output: " + error);
    CookedTexture fromKtx;
    check(load_cooked_texture((dir / "out_bc1.fusetex").string(), fromKtx, &error) && fromKtx.format == BcFormat::BC1 &&
              fromKtx.levels.size() == 5u,
          "cli KTX2 → fusetex: " + error);
    Ktx2Image normal;
    check(read_ktx2_file((dir / "normal.ktx2").string(), normal, &error) && normal.vk_format == vk_format::kBc5Unorm,
          "cli normal map → BC5 KTX2: " + error);
}

} // namespace

int main(int argc, char** argv) {
    test_ktx_create_reference();
    test_writer_reader_round_trip();
    test_cooked_texture_round_trip();
    test_uncompressed_sources();
    test_rejections();
    test_fuse_cook_cli(argc > 1 ? argv[1] : nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_ktx2: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_asset_ktx2: all checks passed\n");
    return EXIT_SUCCESS;
}
