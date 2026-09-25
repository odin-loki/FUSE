#include <fuse/cook/texture_cook.hpp>

#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/ktx2.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#if defined(FUSE_HAS_STB_IMAGE)
#include "stb_image.h"
#endif

namespace fuse::cook {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void set_failure(CookFailure* failure, CookFailure value) {
    if (failure != nullptr) {
        *failure = value;
    }
}

std::string lower_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool write_bytes(const std::string& output_path, const std::vector<u8>& bytes) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool read_bytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

u32 mip_count(u32 width, u32 height) {
    u32 count = 1;
    while (width > 1u || height > 1u) {
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
        ++count;
    }
    return count;
}

/// Tangent-space normal map texel (RGB8, 128 = 0) → unit vector (degenerate texels become +Z).
void decode_normal(const u8* rgb, f32 n[3]) {
    f32 len = 0.f;
    for (u32 d = 0; d < 3u; ++d) {
        n[d] = static_cast<f32>(rgb[d]) / 127.5f - 1.f;
        len += n[d] * n[d];
    }
    len = std::sqrt(len);
    if (len < 1e-6f) {
        n[0] = 0.f;
        n[1] = 0.f;
        n[2] = 1.f;
        return;
    }
    for (u32 d = 0; d < 3u; ++d) {
        n[d] /= len;
    }
}

void encode_normal(const f32 n[3], u8* rgb) {
    for (u32 d = 0; d < 3u; ++d) {
        const f32 v = std::clamp((n[d] * 0.5f + 0.5f) * 255.f, 0.f, 255.f);
        rgb[d] = static_cast<u8>(std::lround(v));
    }
}

/// Normal-map mip chain: box-average unit vectors, renormalise. Level 0 is renormalised too.
std::vector<Bc7RgbaImage> build_normal_mip_chain(const u8* rgba, u32 width, u32 height, bool mipmaps) {
    std::vector<std::vector<f32>> vectors;
    std::vector<Bc7RgbaImage> chain;
    std::vector<f32> base(static_cast<usize>(width) * height * 3u);
    for (usize i = 0; i < static_cast<usize>(width) * height; ++i) {
        decode_normal(rgba + i * 4u, &base[i * 3u]);
    }
    u32 w = width;
    u32 h = height;
    std::vector<f32> current = std::move(base);
    for (;;) {
        Bc7RgbaImage level;
        level.width = w;
        level.height = h;
        level.rgba.resize(static_cast<usize>(w) * h * 4u);
        for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
            encode_normal(&current[i * 3u], &level.rgba[i * 4u]);
            level.rgba[i * 4u + 3u] = 255u;
        }
        chain.push_back(std::move(level));
        if (!mipmaps || (w == 1u && h == 1u)) {
            break;
        }
        const u32 nw = std::max(1u, w / 2u);
        const u32 nh = std::max(1u, h / 2u);
        std::vector<f32> next(static_cast<usize>(nw) * nh * 3u);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                const u32 xs[2] = {std::min(x * 2u, w - 1u), std::min(x * 2u + 1u, w - 1u)};
                const u32 ys[2] = {std::min(y * 2u, h - 1u), std::min(y * 2u + 1u, h - 1u)};
                f32 sum[3] = {};
                for (u32 sy : ys) {
                    for (u32 sx : xs) {
                        for (u32 d = 0; d < 3u; ++d) {
                            sum[d] += current[(static_cast<usize>(sy) * w + sx) * 3u + d];
                        }
                    }
                }
                f32 len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                f32* dst = &next[(static_cast<usize>(y) * nw + x) * 3u];
                if (len < 1e-6f) {
                    dst[0] = 0.f;
                    dst[1] = 0.f;
                    dst[2] = 1.f;
                } else {
                    for (u32 d = 0; d < 3u; ++d) {
                        dst[d] = sum[d] / len;
                    }
                }
            }
        }
        current = std::move(next);
        w = nw;
        h = nh;
    }
    return chain;
}

struct HalfLevel {
    u32 width = 0;
    u32 height = 0;
    std::vector<u16> rgba16f;
};

/// HDR mip chain: box filter in linear float, stored as half floats.
std::vector<HalfLevel> build_half_mip_chain(const u16* rgba16f, u32 width, u32 height, bool mipmaps) {
    std::vector<HalfLevel> chain;
    HalfLevel base;
    base.width = width;
    base.height = height;
    base.rgba16f.assign(rgba16f, rgba16f + static_cast<usize>(width) * height * 4u);
    chain.push_back(std::move(base));
    while (mipmaps && (chain.back().width > 1u || chain.back().height > 1u)) {
        const HalfLevel& src = chain.back();
        HalfLevel dst;
        dst.width = std::max(1u, src.width / 2u);
        dst.height = std::max(1u, src.height / 2u);
        dst.rgba16f.resize(static_cast<usize>(dst.width) * dst.height * 4u);
        for (u32 y = 0; y < dst.height; ++y) {
            for (u32 x = 0; x < dst.width; ++x) {
                const u32 xs[2] = {std::min(x * 2u, src.width - 1u), std::min(x * 2u + 1u, src.width - 1u)};
                const u32 ys[2] = {std::min(y * 2u, src.height - 1u), std::min(y * 2u + 1u, src.height - 1u)};
                for (u32 c = 0; c < 4u; ++c) {
                    f32 sum = 0.f;
                    for (u32 sy : ys) {
                        for (u32 sx : xs) {
                            sum += half_to_float(src.rgba16f[(static_cast<usize>(sy) * src.width + sx) * 4u + c]);
                        }
                    }
                    dst.rgba16f[(static_cast<usize>(y) * dst.width + x) * 4u + c] = float_to_half(sum * 0.25f);
                }
            }
        }
        chain.push_back(std::move(dst));
    }
    return chain;
}

TextureCookOptions normalized_options(TextureCookOptions options) {
    if (options.normal_map) {
        options.format = BcFormat::BC5;
    }
    if (options.format != BcFormat::BC1 && options.format != BcFormat::BC7) {
        options.srgb = false;
    }
    return options;
}

std::string format_texel_m(f32 value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

#if defined(FUSE_HAS_STB_IMAGE)
/// stb_image decode with the strict-cook classification (zero-size, oversize, corrupt).
bool decode_stb_source(const std::string& input_path, TextureSource& out, CookFailure* failure, std::string* error,
                       bool keep_hdr) {
    // stb flags a zero width/height header as "0-pixel image". Its "too large" (> 2^24 pixels on a
    // side, or a size overflow) comes from garbage headers, so it stays CorruptImage; plausible
    // oversize images are caught by the explicit kMaxCookTextureDimension check below.
    auto classify = [](const char* reason) {
        const std::string text = reason != nullptr ? reason : "";
        return text == "0-pixel image" ? CookFailure::InvalidImageDimensions : CookFailure::CorruptImage;
    };

    // stbi_info tries every loader and reports the *last* failure, so a PNG whose IHDR says 0x0 comes
    // back as "unknown image type". Read the PNG IHDR directly to classify that case precisely.
    {
        std::ifstream probe(input_path, std::ios::binary);
        unsigned char head[24] = {};
        if (probe.read(reinterpret_cast<char*>(head), sizeof(head)) &&
            std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(head + 12, "IHDR", 4) == 0) {
            auto be32 = [&head](int at) {
                return (static_cast<u32>(head[at]) << 24) | (static_cast<u32>(head[at + 1]) << 16) |
                       (static_cast<u32>(head[at + 2]) << 8) | static_cast<u32>(head[at + 3]);
            };
            const u32 pngWidth = be32(16);
            const u32 pngHeight = be32(20);
            if (pngWidth == 0u || pngHeight == 0u) {
                set_error(error, "texture rejected: dimensions " + std::to_string(pngWidth) + "x" +
                                     std::to_string(pngHeight) + " (zero-size image)");
                set_failure(failure, CookFailure::InvalidImageDimensions);
                return false;
            }
        }
    }

    // Header probe first: reject zero-size / oversize images before allocating pixels for them.
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(input_path.c_str(), &width, &height, &channels) == 0) {
        const char* reason = stbi_failure_reason();
        set_error(error, std::string("texture decode failed: ") + (reason != nullptr ? reason : "unknown"));
        set_failure(failure, classify(reason));
        return false;
    }
    if (width <= 0 || height <= 0 || static_cast<u32>(width) > kMaxCookTextureDimension ||
        static_cast<u32>(height) > kMaxCookTextureDimension) {
        set_error(error, "texture rejected: dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                             " outside 1.." + std::to_string(kMaxCookTextureDimension));
        set_failure(failure, CookFailure::InvalidImageDimensions);
        return false;
    }

    const usize texels = static_cast<usize>(width) * static_cast<usize>(height);
    if (keep_hdr && stbi_is_hdr(input_path.c_str()) != 0) {
        float* pixels = stbi_loadf(input_path.c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) {
            const char* reason = stbi_failure_reason();
            set_error(error, std::string("texture decode failed: ") + (reason != nullptr ? reason : "unknown"));
            set_failure(failure, classify(reason));
            return false;
        }
        out = TextureSource{};
        out.width = static_cast<u32>(width);
        out.height = static_cast<u32>(height);
        out.rgba16f.resize(texels * 4u);
        for (usize i = 0; i < texels * 4u; ++i) {
            out.rgba16f[i] = float_to_half(pixels[i]);
        }
        stbi_image_free(pixels);
        return true;
    }
    unsigned char* pixels = stbi_load(input_path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        set_error(error, std::string("texture decode failed: ") + (reason != nullptr ? reason : "unknown"));
        set_failure(failure, classify(reason));
        return false;
    }
    out = TextureSource{};
    out.width = static_cast<u32>(width);
    out.height = static_cast<u32>(height);
    out.rgba8.assign(pixels, pixels + texels * 4u);
    stbi_image_free(pixels);
    return true;
}
#endif

CookStubWriteResult fail(CookFailure failure, const std::string& note) {
    CookStubWriteResult result;
    result.failure = failure;
    result.note = note;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// W0.3: multi-format cook
// ---------------------------------------------------------------------------------------------

CookStubWriteResult cook_texture_image(const TextureSource& source, const TextureCookOptions& requested,
                                       CookedTexture& out) {
    const TextureCookOptions options = normalized_options(requested);
    if (source.width == 0u || source.height == 0u || source.width > kMaxCookTextureDimension ||
        source.height > kMaxCookTextureDimension) {
        return fail(CookFailure::InvalidImageDimensions,
                    "texture dimensions " + std::to_string(source.width) + "x" + std::to_string(source.height) +
                        " invalid (must be 1.." + std::to_string(kMaxCookTextureDimension) + ")");
    }
    if (source.layers == 0u || source.layers > kMaxCookTextureLayers) {
        return fail(CookFailure::InvalidImageDimensions,
                    "texture layer count " + std::to_string(source.layers) + " invalid (must be 1.." +
                        std::to_string(kMaxCookTextureLayers) + ")");
    }
    if (options.cube && (source.layers % 6u != 0u || source.width != source.height)) {
        return fail(CookFailure::InvalidImageDimensions,
                    "cube map needs square faces and a layer count that is a multiple of 6");
    }
    const usize layerTexels = static_cast<usize>(source.width) * source.height;
    const usize texels = layerTexels * source.layers;
    const bool haveLdr = source.rgba8.size() == texels * 4u;
    const bool haveHdr = source.rgba16f.size() == texels * 4u;
    if (options.format == BcFormat::BC6H ? !(haveLdr || haveHdr) : !haveLdr) {
        return fail(CookFailure::InvalidArgument,
                    std::string("texture source pixels missing or sized wrongly for ") + bc_format_name(options.format));
    }
    if (!std::isfinite(options.texel_m) || options.texel_m < 0.f) {
        return fail(CookFailure::InvalidArgument, "texel_m must be finite and >= 0");
    }

    CookedTexture cooked;
    cooked.format = options.format;
    cooked.compression = bc_format_name(options.format);
    cooked.width = source.width;
    cooked.height = source.height;
    cooked.layers = source.layers;
    cooked.cube = options.cube;
    cooked.srgb = options.srgb;
    cooked.normal_map = options.normal_map;
    cooked.texel_m = options.texel_m;
    const u32 levelCount = options.mipmaps ? mip_count(source.width, source.height) : 1u;
    cooked.levels.resize(levelCount);
    {
        u32 w = source.width;
        u32 h = source.height;
        for (CookedTexture::Level& level : cooked.levels) {
            level.width = w;
            level.height = h;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
    }

    for (u32 layer = 0; layer < source.layers; ++layer) {
        std::vector<BcSourceImage> images(levelCount);
        if (options.format == BcFormat::BC6H) {
            std::vector<u16> halves;
            if (haveHdr) {
                halves.assign(source.rgba16f.begin() + static_cast<std::ptrdiff_t>(layerTexels * 4u * layer),
                              source.rgba16f.begin() + static_cast<std::ptrdiff_t>(layerTexels * 4u * (layer + 1u)));
            } else {
                halves.resize(layerTexels * 4u);
                for (usize i = 0; i < layerTexels * 4u; ++i) {
                    halves[i] = float_to_half(static_cast<f32>(source.rgba8[layerTexels * 4u * layer + i]) / 255.f);
                }
            }
            std::vector<HalfLevel> chain = build_half_mip_chain(halves.data(), source.width, source.height,
                                                                options.mipmaps);
            for (u32 l = 0; l < levelCount; ++l) {
                images[l].width = chain[l].width;
                images[l].height = chain[l].height;
                images[l].rgba16f = std::move(chain[l].rgba16f);
            }
        } else {
            const u8* base = source.rgba8.data() + layerTexels * 4u * layer;
            std::vector<Bc7RgbaImage> chain;
            if (options.normal_map) {
                chain = build_normal_mip_chain(base, source.width, source.height, options.mipmaps);
            } else if (options.mipmaps) {
                chain = build_rgba_mip_chain(base, source.width, source.height);
            } else {
                Bc7RgbaImage single;
                single.width = source.width;
                single.height = source.height;
                single.rgba.assign(base, base + layerTexels * 4u);
                chain.push_back(std::move(single));
            }
            for (u32 l = 0; l < levelCount; ++l) {
                images[l].width = chain[l].width;
                images[l].height = chain[l].height;
                images[l].rgba8 = std::move(chain[l].rgba);
            }
        }
        for (u32 l = 0; l < levelCount; ++l) {
            std::vector<u8> blocks;
            std::string error;
            if (!encode_bc_image(options.format, images[l], blocks, &error)) {
                return fail(CookFailure::InvalidArgument, error);
            }
            cooked.levels[l].blocks.insert(cooked.levels[l].blocks.end(), blocks.begin(), blocks.end());
        }
    }
    out = std::move(cooked);
    CookStubWriteResult result;
    result.ok = true;
    result.note = std::string(bc_format_name(options.format)) + " encoded, layers=" + std::to_string(source.layers) +
                  ", mip_levels=" + std::to_string(levelCount);
    return result;
}

std::vector<u8> serialize_cooked_texture(const CookedTexture& texture) {
    u64 blocks = 0;
    for (const CookedTexture::Level& level : texture.levels) {
        blocks += static_cast<u64>(bc_block_count(level.width, level.height)) * texture.layers;
    }
    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "format_version=2\n";
    header << "hook=fuse_bcn\n";
    header << "compression=" << bc_format_name(texture.format) << "\n";
    header << "width=" << texture.width << "\n";
    header << "height=" << texture.height << "\n";
    header << "layers=" << texture.layers << "\n";
    header << "cube=" << (texture.cube ? 1 : 0) << "\n";
    header << "srgb=" << (texture.srgb ? 1 : 0) << "\n";
    header << "block_bytes=" << bc_block_bytes(texture.format) << "\n";
    header << "blocks=" << blocks << "\n";
    header << "mipmaps=" << (texture.levels.size() > 1u ? "on" : "off") << "\n";
    header << "mip_levels=" << texture.levels.size() << "\n";
    if (texture.normal_map) {
        header << "normal_convention=gl\n";
    }
    if (texture.texel_m > 0.f) {
        header << "texel_m=" << format_texel_m(texture.texel_m) << "\n";
    }
    header << "DATA\n";
    const std::string text = header.str();
    std::vector<u8> out(text.begin(), text.end());
    for (const CookedTexture::Level& level : texture.levels) {
        out.insert(out.end(), level.blocks.begin(), level.blocks.end());
    }
    return out;
}

bool parse_cooked_texture(const u8* data, usize size, CookedTexture& out, std::string* error) {
    out = CookedTexture{};
    const std::string bytes(reinterpret_cast<const char*>(data), data != nullptr ? size : 0u);
    const std::size_t dataMarker = bytes.find("\nDATA\n");
    if (bytes.rfind("FUSETEX_BC7\n", 0) != 0 || dataMarker == std::string::npos) {
        set_error(error, "cooked texture header invalid");
        return false;
    }

    u32 width = 0;
    u32 height = 0;
    u64 blocks = 0;
    u32 mipLevels = 0;
    u32 layers = 1;
    u32 blockBytes = 0;
    u32 formatVersion = 1;
    bool cube = false;
    bool srgbKey = false;
    bool srgb = true;
    std::string compression = "BC7";
    CookedTexture parsed;
    std::istringstream header(bytes.substr(0, dataMarker));
    std::string line;
    while (std::getline(header, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "width") {
                width = static_cast<u32>(std::stoul(value));
            } else if (key == "height") {
                height = static_cast<u32>(std::stoul(value));
            } else if (key == "blocks") {
                blocks = static_cast<u64>(std::stoull(value));
            } else if (key == "mip_levels") {
                mipLevels = static_cast<u32>(std::stoul(value));
            } else if (key == "compression") {
                compression = value;
            } else if (key == "layers") {
                layers = static_cast<u32>(std::stoul(value));
            } else if (key == "block_bytes") {
                blockBytes = static_cast<u32>(std::stoul(value));
            } else if (key == "format_version") {
                formatVersion = static_cast<u32>(std::stoul(value));
            } else if (key == "cube") {
                cube = std::stoul(value) != 0u;
            } else if (key == "srgb") {
                srgbKey = true;
                srgb = std::stoul(value) != 0u;
            } else if (key == "normal_convention") {
                if (value != "gl") {
                    set_error(error, "cooked texture normal_convention '" + value + "' unsupported (only gl)");
                    return false;
                }
                parsed.normal_map = true;
            } else if (key == "texel_m") {
                parsed.texel_m = std::stof(value);
            }
        } catch (...) {
            set_error(error, "cooked texture header field '" + key + "' invalid");
            return false;
        }
    }
    if (formatVersion < 1u || formatVersion > 2u) {
        set_error(error, "cooked texture format_version " + std::to_string(formatVersion) + " unsupported");
        return false;
    }
    BcFormat format = BcFormat::BC7;
    if (!parse_bc_format(compression, format)) {
        set_error(error, "cooked texture compression '" + compression + "' unsupported");
        return false;
    }
    if (formatVersion == 1u) {
        // Version 1 files (BC7 mode 6, or the ispc hook's BC5/BC7) always stored 16-byte blocks.
        blockBytes = blockBytes == 0u ? 16u : blockBytes;
        if (format != BcFormat::BC7 && format != BcFormat::BC5) {
            set_error(error, "cooked texture v1 compression '" + compression + "' unsupported");
            return false;
        }
    }
    if (blockBytes != bc_block_bytes(format)) {
        set_error(error, "cooked texture block_bytes does not match " + compression);
        return false;
    }
    if (width == 0u || height == 0u || width > kMaxCookTextureDimension || height > kMaxCookTextureDimension ||
        mipLevels == 0u || mipLevels > mip_count(width, height) || layers == 0u || layers > kMaxCookTextureLayers) {
        set_error(error, "cooked texture dimensions invalid");
        return false;
    }
    if (cube && (layers % 6u != 0u || width != height)) {
        set_error(error, "cooked texture cube layout invalid");
        return false;
    }
    if (!std::isfinite(parsed.texel_m) || parsed.texel_m < 0.f) {
        set_error(error, "cooked texture texel_m invalid");
        return false;
    }

    const std::size_t dataOffset = dataMarker + 6u;
    std::size_t cursor = dataOffset;
    u32 levelWidth = width;
    u32 levelHeight = height;
    u64 counted = 0;
    for (u32 level = 0; level < mipLevels; ++level) {
        const u64 levelBlocks = static_cast<u64>(bc_block_count(levelWidth, levelHeight)) * layers;
        const u64 levelBytes = levelBlocks * blockBytes;
        if (static_cast<u64>(cursor) + levelBytes > bytes.size()) {
            set_error(error, "cooked texture block data truncated");
            return false;
        }
        CookedTexture::Level entry;
        entry.width = levelWidth;
        entry.height = levelHeight;
        entry.blocks.assign(reinterpret_cast<const u8*>(bytes.data()) + cursor,
                            reinterpret_cast<const u8*>(bytes.data()) + cursor + levelBytes);
        parsed.levels.push_back(std::move(entry));
        cursor += static_cast<std::size_t>(levelBytes);
        counted += levelBlocks;
        levelWidth = levelWidth > 1u ? levelWidth / 2u : 1u;
        levelHeight = levelHeight > 1u ? levelHeight / 2u : 1u;
    }
    if (cursor != bytes.size() || counted != blocks) {
        set_error(error, "cooked texture block count mismatch");
        return false;
    }
    parsed.compression = compression;
    parsed.format = format;
    parsed.width = width;
    parsed.height = height;
    parsed.layers = layers;
    parsed.cube = cube;
    parsed.srgb = srgbKey ? srgb : (format == BcFormat::BC7 || format == BcFormat::BC1);
    if (parsed.normal_map && format != BcFormat::BC5) {
        set_error(error, "cooked texture normal map must be BC5");
        return false;
    }
    out = std::move(parsed);
    return true;
}

CookStubWriteResult write_cooked_texture(const CookedTexture& texture, const std::string& output_path) {
    if (output_path.empty()) {
        return fail(CookFailure::InvalidArgument, "missing output path");
    }
    std::vector<u8> bytes;
    if (lower_extension(output_path) == ".ktx2") {
        Ktx2Image image;
        std::string error;
        if (!cooked_texture_to_ktx2(texture, image, &error) || !write_ktx2(image, bytes, &error)) {
            return fail(CookFailure::InvalidArgument, "ktx2 export failed: " + error);
        }
    } else {
        bytes = serialize_cooked_texture(texture);
    }
    if (!write_bytes(output_path, bytes)) {
        return fail(CookFailure::WriteFailed, "unable to write cooked texture output");
    }
    CookStubWriteResult result;
    result.ok = true;
    result.byteCount = static_cast<u32>(bytes.size());
    result.note = std::string(bc_format_name(texture.format)) + " written, layers=" + std::to_string(texture.layers) +
                  ", mip_levels=" + std::to_string(texture.levels.size()) +
                  (lower_extension(output_path) == ".ktx2" ? " (ktx2)" : "");
    return result;
}

bool load_texture_source(const std::string& input_path, TextureSource& out, CookFailure* failure, std::string* error,
                         bool keep_hdr) {
    set_failure(failure, CookFailure::None);
    if (input_path.empty()) {
        set_failure(failure, CookFailure::InvalidArgument);
        set_error(error, "missing input path");
        return false;
    }
    if (lower_extension(input_path) == ".ktx2") {
        Ktx2Image image;
        std::string why;
        if (!read_ktx2_file(input_path, image, &why)) {
            set_failure(failure, CookFailure::CorruptImage);
            set_error(error, "ktx2 import failed: " + why);
            return false;
        }
        if (!ktx2_to_texture_source(image, out, &why)) {
            set_failure(failure, CookFailure::CorruptImage);
            set_error(error, "ktx2 import failed: " + why);
            return false;
        }
        return true;
    }
#if defined(FUSE_HAS_STB_IMAGE)
    return decode_stb_source(input_path, out, failure, error, keep_hdr);
#else
    (void)keep_hdr;
    set_failure(failure, CookFailure::ImporterUnavailable);
    set_error(error, "stb_image unavailable (library not linked)");
    return false;
#endif
}

CookStubWriteResult cook_texture_file(const std::string& input_path, const std::string& output_path,
                                      const TextureCookOptions& requested) {
    if (input_path.empty() || output_path.empty()) {
        return fail(CookFailure::InvalidArgument, "missing input or output path");
    }
    const TextureCookOptions options = normalized_options(requested);
    const std::string inExt = lower_extension(input_path);

    // Transport pass-through: an already block-compressed KTX2 / .fusetex is re-containered, never
    // re-encoded (transcoding between block formats would compound error; re-cook from the source).
    bool passThrough = inExt == ".fusetex";
    Ktx2Image ktx;
    if (inExt == ".ktx2") {
        std::string why;
        if (!read_ktx2_file(input_path, ktx, &why)) {
            return fail(CookFailure::CorruptImage, "ktx2 import failed: " + why);
        }
        passThrough = ktx2_format_is_block_compressed(ktx.vk_format);
    }
    if (passThrough) {
        CookedTexture texture;
        std::string why;
        const bool loaded = inExt == ".ktx2" ? ktx2_to_cooked_texture(ktx, texture, &why)
                                             : load_cooked_texture(input_path, texture, &why);
        if (!loaded) {
            return fail(CookFailure::CorruptImage, "texture import failed: " + why);
        }
        if (texture.format != options.format) {
            return fail(CookFailure::InvalidArgument,
                        std::string("source is already ") + bc_format_name(texture.format) + ", requested " +
                            bc_format_name(options.format) + " (transcoding between block formats is not supported)");
        }
        return write_cooked_texture(texture, output_path);
    }

    TextureSource source;
    if (inExt == ".ktx2") {
        std::string why;
        if (!ktx2_to_texture_source(ktx, source, &why)) {
            return fail(CookFailure::CorruptImage, "ktx2 import failed: " + why);
        }
    } else {
        CookFailure failure = CookFailure::None;
        std::string why;
        if (!load_texture_source(input_path, source, &failure, &why, options.format == BcFormat::BC6H)) {
            return fail(failure, why);
        }
    }

    // Plain 2D sRGB BC7 keeps the original (version 1) container byte for byte.
    const bool legacy = options.format == BcFormat::BC7 && options.srgb && !options.normal_map && !options.cube &&
                        options.texel_m == 0.f && source.layers == 1u && !source.rgba8.empty() &&
                        lower_extension(output_path) != ".ktx2";
    if (legacy) {
        return write_texture_bc7_rgba(source.rgba8.data(), source.width, source.height, output_path, options.mipmaps);
    }
    if (options.format != BcFormat::BC6H && source.rgba8.empty()) {
        return fail(CookFailure::InvalidArgument,
                    std::string("HDR source cannot be cooked to ") + bc_format_name(options.format) + " (use BC6H)");
    }
    CookedTexture texture;
    CookStubWriteResult cooked = cook_texture_image(source, options, texture);
    if (!cooked.ok) {
        return cooked;
    }
    return write_cooked_texture(texture, output_path);
}

// ---------------------------------------------------------------------------------------------
// Original BC7 path (version 1 container)
// ---------------------------------------------------------------------------------------------

CookStubWriteResult write_texture_bc7_rgba(const u8* rgba, u32 width, u32 height, const std::string& output_path,
                                           bool mipmaps, const char* hook) {
    CookStubWriteResult result;
    if (rgba == nullptr || output_path.empty()) {
        result.note = "invalid rgba input or output path";
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
    if (width == 0u || height == 0u || width > kMaxCookTextureDimension || height > kMaxCookTextureDimension) {
        result.note = "texture dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                      " invalid (must be 1.." + std::to_string(kMaxCookTextureDimension) + ")";
        result.failure = CookFailure::InvalidImageDimensions;
        return result;
    }

    std::vector<Bc7RgbaImage> levels;
    if (mipmaps) {
        levels = build_rgba_mip_chain(rgba, width, height);
    } else {
        Bc7RgbaImage base;
        base.width = width;
        base.height = height;
        base.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
        levels.push_back(std::move(base));
    }

    std::string blocks;
    u32 totalBlocks = 0;
    for (const Bc7RgbaImage& level : levels) {
        std::vector<u8> encoded;
        const Bc7EncodeResult levelResult = encode_bc7_rgba8(level.rgba.data(), level.width, level.height, encoded);
        if (!levelResult.ok) {
            result.note = levelResult.note;
            result.failure = CookFailure::InvalidArgument;
            return result;
        }
        totalBlocks += levelResult.blockCount;
        blocks.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    }

    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "hook=" << (hook != nullptr ? hook : "bc7_mode6") << "\n";
    header << "compression=BC7\n";
    header << "width=" << width << "\n";
    header << "height=" << height << "\n";
    header << "blocks=" << totalBlocks << "\n";
    header << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    header << "mip_levels=" << levels.size() << "\n";
    header << "mode=6\n";
    header << "DATA\n";
    const std::string payload = header.str() + blocks;

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "unable to write BC7 texture output";
        result.failure = CookFailure::WriteFailed;
        return result;
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.failure = result.ok ? CookFailure::None : CookFailure::WriteFailed;
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? ("bc7 encoded, blocks=" + std::to_string(totalBlocks) +
                               ", mip_levels=" + std::to_string(levels.size()))
                            : "bc7 write failed";
    return result;
}

CookStubWriteResult cook_texture_bc7_file(const std::string& input_path, const std::string& output_path,
                                          bool mipmaps) {
    if (input_path.empty() || output_path.empty()) {
        return fail(CookFailure::InvalidArgument, "missing input or output path");
    }
#if defined(FUSE_HAS_STB_IMAGE)
    TextureSource source;
    CookFailure failure = CookFailure::None;
    std::string why;
    if (!decode_stb_source(input_path, source, &failure, &why, false)) {
        return fail(failure, why);
    }
    return write_texture_bc7_rgba(source.rgba8.data(), source.width, source.height, output_path, mipmaps);
#else
    (void)mipmaps;
    return fail(CookFailure::ImporterUnavailable, "stb_image unavailable (library not linked)");
#endif
}

bool load_cooked_texture(const std::string& path, CookedTexture& out, std::string* error) {
    out = CookedTexture{};
    std::vector<u8> bytes;
    if (!read_bytes(path, bytes)) {
        set_error(error, "cooked texture unreadable");
        return false;
    }
    if (lower_extension(path) == ".ktx2") {
        Ktx2Image image;
        return read_ktx2(bytes.data(), bytes.size(), image, error) && ktx2_to_cooked_texture(image, out, error);
    }
    return parse_cooked_texture(bytes.data(), bytes.size(), out, error);
}

} // namespace fuse::cook
