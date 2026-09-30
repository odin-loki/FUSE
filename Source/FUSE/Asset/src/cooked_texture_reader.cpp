// UNI-U7-ASSET-1: runtime `.fusetex` reader and BCn format helpers, moved from Tools/FUSE/Cook
// (texture_cook.cpp, bcn_encoder.cpp; asset plan W0.3). Behaviour is unchanged: the cook's texture gates
// (fuse_asset_texture_bcn, fuse_asset_ktx2) exercise this code through the fuse::cook re-exports.

#include <fuse/asset/cooked_texture.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fuse::asset {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

const char* bc_format_name(BcFormat format) {
    switch (format) {
    case BcFormat::BC1:
        return "BC1";
    case BcFormat::BC4:
        return "BC4";
    case BcFormat::BC5:
        return "BC5";
    case BcFormat::BC6H:
        return "BC6H";
    case BcFormat::BC7:
        return "BC7";
    }
    return "BC7";
}

bool parse_bc_format(const std::string& text, BcFormat& out) {
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static constexpr BcFormat kAll[] = {BcFormat::BC1, BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7};
    for (BcFormat format : kAll) {
        if (upper == bc_format_name(format)) {
            out = format;
            return true;
        }
    }
    return false;
}

u32 bc_block_bytes(BcFormat format) {
    return (format == BcFormat::BC1 || format == BcFormat::BC4) ? 8u : 16u;
}

u32 bc_block_count(u32 width, u32 height) {
    return ((width + 3u) / 4u) * ((height + 3u) / 4u);
}

u32 bc_mip_count(u32 width, u32 height) {
    u32 count = 1;
    while (width > 1u || height > 1u) {
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
        ++count;
    }
    return count;
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
        mipLevels == 0u || mipLevels > bc_mip_count(width, height) || layers == 0u || layers > kMaxCookTextureLayers) {
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

bool read_cooked_texture_file(const std::string& path, CookedTexture& out, std::string* error) {
    out = CookedTexture{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cooked texture unreadable");
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_cooked_texture(bytes.data(), bytes.size(), out, error);
}

} // namespace fuse::asset
