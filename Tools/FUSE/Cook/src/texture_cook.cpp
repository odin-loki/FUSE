#include <fuse/cook/texture_cook.hpp>

#include <fuse/cook/bc7_encoder.hpp>

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

u32 level_block_count(u32 width, u32 height) {
    return (bc7_padded_dimension(width) / 4u) * (bc7_padded_dimension(height) / 4u);
}

} // namespace

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
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing input or output path";
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
#if defined(FUSE_HAS_STB_IMAGE)
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
                result.note = "texture rejected: dimensions " + std::to_string(pngWidth) + "x" +
                              std::to_string(pngHeight) + " (zero-size image)";
                result.failure = CookFailure::InvalidImageDimensions;
                return result;
            }
        }
    }

    // Header probe first: reject zero-size / oversize images before allocating pixels for them.
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(input_path.c_str(), &width, &height, &channels) == 0) {
        const char* reason = stbi_failure_reason();
        result.note = std::string("texture decode failed: ") + (reason != nullptr ? reason : "unknown");
        result.failure = classify(reason);
        return result;
    }
    if (width <= 0 || height <= 0 || static_cast<u32>(width) > kMaxCookTextureDimension ||
        static_cast<u32>(height) > kMaxCookTextureDimension) {
        result.note = "texture rejected: dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                      " outside 1.." + std::to_string(kMaxCookTextureDimension);
        result.failure = CookFailure::InvalidImageDimensions;
        return result;
    }

    unsigned char* pixels = stbi_load(input_path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        result.note = std::string("texture decode failed: ") + (reason != nullptr ? reason : "unknown");
        result.failure = classify(reason);
        return result;
    }
    result = write_texture_bc7_rgba(pixels, static_cast<u32>(width), static_cast<u32>(height), output_path, mipmaps);
    stbi_image_free(pixels);
    return result;
#else
    (void)mipmaps;
    result.note = "stb_image unavailable (library not linked)";
    result.failure = CookFailure::ImporterUnavailable;
    return result;
#endif
}

bool load_cooked_texture(const std::string& path, CookedTexture& out, std::string* error) {
    out = CookedTexture{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cooked texture unreadable");
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t dataMarker = bytes.find("\nDATA\n");
    if (bytes.rfind("FUSETEX_BC7\n", 0) != 0 || dataMarker == std::string::npos) {
        set_error(error, "cooked texture header invalid");
        return false;
    }

    u32 width = 0;
    u32 height = 0;
    u32 blocks = 0;
    u32 mipLevels = 0;
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
                blocks = static_cast<u32>(std::stoul(value));
            } else if (key == "mip_levels") {
                mipLevels = static_cast<u32>(std::stoul(value));
            } else if (key == "compression") {
                out.compression = value;
            }
        } catch (...) {
            set_error(error, "cooked texture header field '" + key + "' invalid");
            return false;
        }
    }
    if (width == 0u || height == 0u || mipLevels == 0u || mipLevels > 32u) {
        set_error(error, "cooked texture dimensions invalid");
        return false;
    }

    const std::size_t dataOffset = dataMarker + 6u;
    std::size_t cursor = dataOffset;
    u32 levelWidth = width;
    u32 levelHeight = height;
    u32 counted = 0;
    for (u32 level = 0; level < mipLevels; ++level) {
        const std::size_t levelBytes = static_cast<std::size_t>(level_block_count(levelWidth, levelHeight)) * 16u;
        if (cursor + levelBytes > bytes.size()) {
            set_error(error, "cooked texture block data truncated");
            out = CookedTexture{};
            return false;
        }
        CookedTexture::Level entry;
        entry.width = levelWidth;
        entry.height = levelHeight;
        entry.blocks.assign(reinterpret_cast<const u8*>(bytes.data()) + cursor,
                            reinterpret_cast<const u8*>(bytes.data()) + cursor + levelBytes);
        out.levels.push_back(std::move(entry));
        cursor += levelBytes;
        counted += level_block_count(levelWidth, levelHeight);
        levelWidth = levelWidth > 1u ? levelWidth / 2u : 1u;
        levelHeight = levelHeight > 1u ? levelHeight / 2u : 1u;
    }
    if (cursor != bytes.size() || counted != blocks) {
        set_error(error, "cooked texture block count mismatch");
        out = CookedTexture{};
        return false;
    }
    return true;
}

} // namespace fuse::cook
