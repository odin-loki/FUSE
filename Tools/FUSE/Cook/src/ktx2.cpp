#include <fuse/cook/ktx2.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>

namespace fuse::cook {

namespace {

constexpr u8 kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr usize kHeaderBytes = 12u + 9u * 4u + 4u * 4u + 2u * 8u; // identifier, header, index = 80
constexpr usize kLevelIndexEntryBytes = 24u;

// Khronos Data Format constants (khr_df.h).
constexpr u32 kModelRgbsda = 1;
constexpr u32 kModelBc1a = 128;
constexpr u32 kModelBc4 = 131;
constexpr u32 kModelBc5 = 132;
constexpr u32 kModelBc6h = 133;
constexpr u32 kModelBc7 = 134;
constexpr u32 kPrimariesBt709 = 1;
constexpr u32 kTransferLinear = 1;
constexpr u32 kTransferSrgb = 2;
constexpr u32 kQualifierFloat = 0x80;
constexpr u32 kQualifierSigned = 0x40;
constexpr u32 kQualifierLinear = 0x10;
constexpr u32 kChannelAlpha = 15;

struct FormatInfo {
    u32 vk_format;
    bool compressed;
    u32 block_bytes; ///< bytes per 4×4 block, or per texel when uncompressed
    u32 type_size;
    bool srgb;
    BcFormat bc;     ///< meaningful when compressed
};

constexpr FormatInfo kFormats[] = {
    {vk_format::kR8G8B8A8Unorm, false, 4, 1, false, BcFormat::BC7},
    {vk_format::kR8G8B8A8Srgb, false, 4, 1, true, BcFormat::BC7},
    {vk_format::kR16G16B16A16Sfloat, false, 8, 2, false, BcFormat::BC7},
    {vk_format::kR32G32B32A32Sfloat, false, 16, 4, false, BcFormat::BC7},
    {vk_format::kBc1RgbUnorm, true, 8, 1, false, BcFormat::BC1},
    {vk_format::kBc1RgbSrgb, true, 8, 1, true, BcFormat::BC1},
    {vk_format::kBc4Unorm, true, 8, 1, false, BcFormat::BC4},
    {vk_format::kBc5Unorm, true, 16, 1, false, BcFormat::BC5},
    {vk_format::kBc6hUfloat, true, 16, 1, false, BcFormat::BC6H},
    {vk_format::kBc7Unorm, true, 16, 1, false, BcFormat::BC7},
    {vk_format::kBc7Srgb, true, 16, 1, true, BcFormat::BC7},
};

const FormatInfo* find_format(u32 vk) {
    for (const FormatInfo& info : kFormats) {
        if (info.vk_format == vk) {
            return &info;
        }
    }
    return nullptr;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void put_u8(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
}
void put_u16(std::vector<u8>& out, u32 v) {
    put_u8(out, v);
    put_u8(out, v >> 8);
}
void put_u32(std::vector<u8>& out, u32 v) {
    for (u32 s = 0; s < 32u; s += 8u) {
        put_u8(out, v >> s);
    }
}
void put_u64(std::vector<u8>& out, u64 v) {
    put_u32(out, static_cast<u32>(v & 0xFFFFFFFFu));
    put_u32(out, static_cast<u32>(v >> 32));
}
void patch_u32(std::vector<u8>& out, usize at, u32 v) {
    for (u32 i = 0; i < 4u; ++i) {
        out[at + i] = static_cast<u8>((v >> (i * 8u)) & 0xFFu);
    }
}
void patch_u64(std::vector<u8>& out, usize at, u64 v) {
    patch_u32(out, at, static_cast<u32>(v & 0xFFFFFFFFu));
    patch_u32(out, at + 4u, static_cast<u32>(v >> 32));
}
u32 get_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u64 get_u64(const u8* p) {
    return static_cast<u64>(get_u32(p)) | (static_cast<u64>(get_u32(p + 4)) << 32);
}
void pad_to(std::vector<u8>& out, usize alignment) {
    while (out.size() % alignment != 0u) {
        out.push_back(0u);
    }
}

u32 dfd_model(const FormatInfo& info) {
    if (!info.compressed) {
        return kModelRgbsda;
    }
    switch (info.bc) {
    case BcFormat::BC1:
        return kModelBc1a;
    case BcFormat::BC4:
        return kModelBc4;
    case BcFormat::BC5:
        return kModelBc5;
    case BcFormat::BC6H:
        return kModelBc6h;
    case BcFormat::BC7:
        return kModelBc7;
    }
    return kModelBc7;
}

/// Basic data format descriptor block for `info` (the DFD libktx / `ktx create` writes for these
/// Vulkan formats: one sample per channel, or per compressed plane).
std::vector<u8> build_dfd(const FormatInfo& info) {
    struct Sample {
        u32 bit_offset;
        u32 bit_length;
        u32 channel;
        u32 lower;
        u32 upper;
    };
    std::vector<Sample> samples;
    if (info.compressed) {
        if (info.bc == BcFormat::BC5) {
            samples.push_back({0, 64, 0, 0, 0xFFFFFFFFu});
            samples.push_back({64, 64, 1, 0, 0xFFFFFFFFu});
        } else if (info.bc == BcFormat::BC6H) {
            samples.push_back({0, 128, kQualifierFloat, 0u, 0x3F800000u}); // UFLOAT: [0.0, 1.0]
        } else {
            samples.push_back({0, info.block_bytes * 8u, 0, 0, 0xFFFFFFFFu});
        }
    } else {
        const u32 bits = info.type_size * 8u;
        for (u32 c = 0; c < 4u; ++c) {
            const u32 channel = c == 3u ? kChannelAlpha : c;
            if (info.type_size == 1u) {
                const u32 linear = (info.srgb && c == 3u) ? kQualifierLinear : 0u;
                samples.push_back({c * bits, bits, channel | linear, 0, 255});
            } else {
                samples.push_back({c * bits, bits, channel | kQualifierFloat | kQualifierSigned, 0xBF800000u,
                                   0x3F800000u});
            }
        }
    }
    const u32 blockSize = 24u + 16u * static_cast<u32>(samples.size());
    std::vector<u8> out;
    put_u32(out, 4u + blockSize); // dfdTotalSize
    put_u32(out, 0u);             // vendorId 0 (Khronos), descriptorType 0 (basic)
    put_u16(out, 2u);             // versionNumber (KDF 1.3)
    put_u16(out, blockSize);
    put_u8(out, dfd_model(info));
    put_u8(out, kPrimariesBt709);
    put_u8(out, info.srgb ? kTransferSrgb : kTransferLinear);
    put_u8(out, 0u); // flags: straight alpha
    const u32 dim = info.compressed ? 3u : 0u;
    put_u8(out, dim);
    put_u8(out, dim);
    put_u8(out, 0u);
    put_u8(out, 0u);
    put_u8(out, info.block_bytes); // bytesPlane0
    for (u32 i = 1; i < 8u; ++i) {
        put_u8(out, 0u);
    }
    for (const Sample& s : samples) {
        put_u16(out, s.bit_offset);
        put_u8(out, s.bit_length - 1u);
        put_u8(out, s.channel);
        put_u32(out, 0u); // samplePosition 0..3
        put_u32(out, s.lower);
        put_u32(out, s.upper);
    }
    return out;
}

u32 level_alignment(const FormatInfo& info) {
    return std::lcm(info.block_bytes, 4u);
}

} // namespace

bool ktx2_format_supported(u32 vk_format) {
    return find_format(vk_format) != nullptr;
}

bool ktx2_format_is_block_compressed(u32 vk_format) {
    const FormatInfo* info = find_format(vk_format);
    return info != nullptr && info->compressed;
}

u64 ktx2_level_bytes(u32 vk_format, u32 width, u32 height, u32 layers_times_faces) {
    const FormatInfo* info = find_format(vk_format);
    if (info == nullptr) {
        return 0;
    }
    const u64 units = info->compressed ? static_cast<u64>(bc_block_count(width, height))
                                       : static_cast<u64>(width) * height;
    return units * info->block_bytes * layers_times_faces;
}

bool write_ktx2(const Ktx2Image& image, std::vector<u8>& out, std::string* error) {
    out.clear();
    const FormatInfo* info = find_format(image.vk_format);
    if (info == nullptr) {
        set_error(error, "vkFormat " + std::to_string(image.vk_format) + " not supported by the KTX2 writer");
        return false;
    }
    if (image.width == 0u || image.height == 0u || image.levels.empty() ||
        (image.face_count != 1u && image.face_count != 6u)) {
        set_error(error, "invalid KTX2 image description");
        return false;
    }
    const u32 layerFaces = std::max(1u, image.layer_count) * image.face_count;
    {
        u32 w = image.width;
        u32 h = image.height;
        for (const std::vector<u8>& level : image.levels) {
            if (level.size() != ktx2_level_bytes(image.vk_format, w, h, layerFaces)) {
                set_error(error, "KTX2 level size does not match its dimensions");
                return false;
            }
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
    }
    const u32 levelCount = static_cast<u32>(image.levels.size());

    out.insert(out.end(), std::begin(kIdentifier), std::end(kIdentifier));
    put_u32(out, image.vk_format);
    put_u32(out, info->type_size);
    put_u32(out, image.width);
    put_u32(out, image.height);
    put_u32(out, 0u); // pixelDepth
    put_u32(out, image.layer_count);
    put_u32(out, image.face_count);
    put_u32(out, levelCount);
    put_u32(out, 0u); // supercompressionScheme: none
    const usize indexAt = out.size();
    for (u32 i = 0; i < 4u; ++i) {
        put_u32(out, 0u); // dfd offset/length, kvd offset/length (patched)
    }
    put_u64(out, 0u); // sgd offset
    put_u64(out, 0u); // sgd length
    const usize levelIndexAt = out.size();
    for (u32 i = 0; i < levelCount * 3u; ++i) {
        put_u64(out, 0u);
    }

    const std::vector<u8> dfd = build_dfd(*info);
    const usize dfdAt = out.size();
    out.insert(out.end(), dfd.begin(), dfd.end());
    patch_u32(out, indexAt + 0u, static_cast<u32>(dfdAt));
    patch_u32(out, indexAt + 4u, static_cast<u32>(dfd.size()));

    std::vector<std::pair<std::string, std::string>> kv = image.key_values;
    if (std::none_of(kv.begin(), kv.end(), [](const auto& e) { return e.first == "KTXwriter"; })) {
        kv.emplace_back("KTXwriter", "FUSE fuse_cook");
    }
    std::stable_sort(kv.begin(), kv.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const usize kvdAt = out.size();
    for (const auto& [key, value] : kv) {
        const u32 length = static_cast<u32>(key.size() + 1u + value.size() + 1u);
        put_u32(out, length);
        out.insert(out.end(), key.begin(), key.end());
        out.push_back(0u);
        out.insert(out.end(), value.begin(), value.end());
        out.push_back(0u);
        pad_to(out, 4u);
    }
    if (!kv.empty()) {
        patch_u32(out, indexAt + 8u, static_cast<u32>(kvdAt));
        patch_u32(out, indexAt + 12u, static_cast<u32>(out.size() - kvdAt));
    }

    // Level data, smallest level first (the specification's recommended order).
    const usize alignment = level_alignment(*info);
    for (u32 i = levelCount; i-- > 0u;) {
        pad_to(out, alignment);
        const usize at = out.size();
        out.insert(out.end(), image.levels[i].begin(), image.levels[i].end());
        patch_u64(out, levelIndexAt + i * kLevelIndexEntryBytes + 0u, at);
        patch_u64(out, levelIndexAt + i * kLevelIndexEntryBytes + 8u, image.levels[i].size());
        patch_u64(out, levelIndexAt + i * kLevelIndexEntryBytes + 16u, image.levels[i].size());
    }
    return true;
}

bool read_ktx2(const u8* data, usize size, Ktx2Image& out, std::string* error) {
    out = Ktx2Image{};
    if (data == nullptr || size < kHeaderBytes || std::memcmp(data, kIdentifier, sizeof(kIdentifier)) != 0) {
        set_error(error, "not a KTX2 file (identifier mismatch or truncated header)");
        return false;
    }
    const u8* h = data + 12;
    const u32 vkFormat = get_u32(h + 0);
    const u32 typeSize = get_u32(h + 4);
    const u32 width = get_u32(h + 8);
    const u32 height = get_u32(h + 12);
    const u32 depth = get_u32(h + 16);
    const u32 layerCount = get_u32(h + 20);
    const u32 faceCount = get_u32(h + 24);
    const u32 levelCountRaw = get_u32(h + 28);
    const u32 supercompression = get_u32(h + 32);
    const u32 dfdOffset = get_u32(h + 36);
    const u32 dfdLength = get_u32(h + 40);
    const u32 kvdOffset = get_u32(h + 44);
    const u32 kvdLength = get_u32(h + 48);
    const u64 sgdLength = get_u64(h + 60);

    if (vkFormat == 0u) {
        set_error(error, "KTX2 vkFormat UNDEFINED (Basis Universal / UASTC) is not supported; transcode offline");
        return false;
    }
    const FormatInfo* info = find_format(vkFormat);
    if (info == nullptr) {
        set_error(error, "KTX2 vkFormat " + std::to_string(vkFormat) + " not supported");
        return false;
    }
    if (supercompression != 0u || sgdLength != 0u) {
        set_error(error, "KTX2 supercompression scheme " + std::to_string(supercompression) +
                             " not supported (only none)");
        return false;
    }
    if (typeSize != info->type_size) {
        set_error(error, "KTX2 typeSize does not match vkFormat");
        return false;
    }
    if (width == 0u || height == 0u || width > kMaxCookTextureDimension || height > kMaxCookTextureDimension) {
        set_error(error, "KTX2 dimensions unsupported (2D textures 1.." + std::to_string(kMaxCookTextureDimension) + ")");
        return false;
    }
    if (depth != 0u) {
        set_error(error, "KTX2 3D textures not supported");
        return false;
    }
    if (faceCount != 1u && faceCount != 6u) {
        set_error(error, "KTX2 faceCount must be 1 or 6");
        return false;
    }
    if (faceCount == 6u && width != height) {
        set_error(error, "KTX2 cube map faces must be square");
        return false;
    }
    if (layerCount > kMaxCookTextureLayers) {
        set_error(error, "KTX2 layerCount too large");
        return false;
    }
    u32 maxLevels = 1;
    for (u32 w = width, hh = height; w > 1u || hh > 1u; w = std::max(1u, w / 2u), hh = std::max(1u, hh / 2u)) {
        ++maxLevels;
    }
    const u32 levelCount = std::max(1u, levelCountRaw);
    if (levelCount > maxLevels) {
        set_error(error, "KTX2 levelCount exceeds the mip chain length");
        return false;
    }
    if (kHeaderBytes + static_cast<u64>(levelCount) * kLevelIndexEntryBytes > size) {
        set_error(error, "KTX2 level index truncated");
        return false;
    }

    // Data format descriptor: must be present, sized consistently, and describe this vkFormat.
    if (dfdLength < 4u + 24u || static_cast<u64>(dfdOffset) + dfdLength > size) {
        set_error(error, "KTX2 data format descriptor missing or out of bounds");
        return false;
    }
    const u8* dfd = data + dfdOffset;
    if (get_u32(dfd) != dfdLength) {
        set_error(error, "KTX2 dfdTotalSize mismatch");
        return false;
    }
    if (dfd[12] != dfd_model(*info)) {
        set_error(error, "KTX2 DFD colour model does not match vkFormat");
        return false;
    }
    const bool dfdSrgb = dfd[14] == kTransferSrgb;
    if (dfdSrgb != info->srgb) {
        set_error(error, "KTX2 DFD transfer function does not match vkFormat");
        return false;
    }

    // Key/value data.
    if (kvdLength != 0u) {
        if (static_cast<u64>(kvdOffset) + kvdLength > size) {
            set_error(error, "KTX2 key/value data out of bounds");
            return false;
        }
        usize at = kvdOffset;
        const usize end = static_cast<usize>(kvdOffset) + kvdLength;
        while (at + 4u <= end) {
            const u32 length = get_u32(data + at);
            at += 4u;
            if (length == 0u || at + length > end) {
                set_error(error, "KTX2 key/value entry out of bounds");
                return false;
            }
            const char* entry = reinterpret_cast<const char*>(data + at);
            const usize keyLen = static_cast<usize>(std::find(entry, entry + length, '\0') - entry);
            if (keyLen == length) {
                set_error(error, "KTX2 key without terminator");
                return false;
            }
            std::string key(entry, keyLen);
            std::string value(entry + keyLen + 1u, length - keyLen - 1u);
            while (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            out.key_values.emplace_back(std::move(key), std::move(value));
            at += length;
            at = (at + 3u) & ~static_cast<usize>(3u);
        }
    }

    const u32 layerFaces = std::max(1u, layerCount) * faceCount;
    const u32 alignment = level_alignment(*info);
    u32 w = width;
    u32 hh = height;
    for (u32 level = 0; level < levelCount; ++level) {
        const u8* entry = data + kHeaderBytes + static_cast<usize>(level) * kLevelIndexEntryBytes;
        const u64 offset = get_u64(entry);
        const u64 length = get_u64(entry + 8);
        const u64 uncompressed = get_u64(entry + 16);
        const u64 expected = ktx2_level_bytes(vkFormat, w, hh, layerFaces);
        if (length != expected || uncompressed != expected) {
            set_error(error, "KTX2 level " + std::to_string(level) + " size mismatch");
            return false;
        }
        if (offset % alignment != 0u || offset > size || length > size - offset) {
            set_error(error, "KTX2 level " + std::to_string(level) + " out of bounds or misaligned");
            return false;
        }
        out.levels.emplace_back(data + offset, data + offset + length);
        w = std::max(1u, w / 2u);
        hh = std::max(1u, hh / 2u);
    }
    out.vk_format = vkFormat;
    out.width = width;
    out.height = height;
    out.layer_count = layerCount;
    out.face_count = faceCount;
    return true;
}

bool read_ktx2_file(const std::string& path, Ktx2Image& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "KTX2 file unreadable");
        out = Ktx2Image{};
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return read_ktx2(bytes.data(), bytes.size(), out, error);
}

bool cooked_texture_to_ktx2(const CookedTexture& texture, Ktx2Image& out, std::string* error) {
    out = Ktx2Image{};
    u32 vk = 0;
    switch (texture.format) {
    case BcFormat::BC1:
        vk = texture.srgb ? vk_format::kBc1RgbSrgb : vk_format::kBc1RgbUnorm;
        break;
    case BcFormat::BC4:
        vk = vk_format::kBc4Unorm;
        break;
    case BcFormat::BC5:
        vk = vk_format::kBc5Unorm;
        break;
    case BcFormat::BC6H:
        vk = vk_format::kBc6hUfloat;
        break;
    case BcFormat::BC7:
        vk = texture.srgb ? vk_format::kBc7Srgb : vk_format::kBc7Unorm;
        break;
    }
    if (texture.levels.empty() || texture.layers == 0u || (texture.cube && texture.layers % 6u != 0u)) {
        set_error(error, "cooked texture has no levels or an invalid layer layout");
        return false;
    }
    out.vk_format = vk;
    out.width = texture.width;
    out.height = texture.height;
    out.face_count = texture.cube ? 6u : 1u;
    const u32 arrayLayers = texture.cube ? texture.layers / 6u : texture.layers;
    out.layer_count = arrayLayers > 1u ? arrayLayers : 0u;
    for (const CookedTexture::Level& level : texture.levels) {
        out.levels.push_back(level.blocks);
    }
    if (texture.normal_map) {
        out.key_values.emplace_back("fuse.normal_convention", "gl");
    }
    if (texture.texel_m > 0.f) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(texture.texel_m));
        out.key_values.emplace_back("fuse.texel_m", text);
    }
    return true;
}

bool ktx2_to_cooked_texture(const Ktx2Image& image, CookedTexture& out, std::string* error) {
    out = CookedTexture{};
    const FormatInfo* info = find_format(image.vk_format);
    if (info == nullptr || !info->compressed) {
        set_error(error, "KTX2 is not block-compressed (import it as a source image instead)");
        return false;
    }
    out.format = info->bc;
    out.compression = bc_format_name(info->bc);
    out.width = image.width;
    out.height = image.height;
    out.cube = image.face_count == 6u;
    out.layers = std::max(1u, image.layer_count) * image.face_count;
    out.srgb = info->srgb;
    for (const auto& [key, value] : image.key_values) {
        if (key == "fuse.normal_convention") {
            if (value != "gl" || info->bc != BcFormat::BC5) {
                set_error(error, "KTX2 fuse.normal_convention must be 'gl' on a BC5 texture");
                out = CookedTexture{};
                return false;
            }
            out.normal_map = true;
        } else if (key == "fuse.texel_m") {
            try {
                out.texel_m = std::stof(value);
            } catch (...) {
                set_error(error, "KTX2 fuse.texel_m invalid");
                out = CookedTexture{};
                return false;
            }
        }
    }
    u32 w = image.width;
    u32 h = image.height;
    for (const std::vector<u8>& level : image.levels) {
        CookedTexture::Level entry;
        entry.width = w;
        entry.height = h;
        entry.blocks = level;
        out.levels.push_back(std::move(entry));
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
    }
    return true;
}

bool ktx2_to_texture_source(const Ktx2Image& image, TextureSource& out, std::string* error) {
    out = TextureSource{};
    const FormatInfo* info = find_format(image.vk_format);
    if (info == nullptr || info->compressed || image.levels.empty()) {
        set_error(error, "KTX2 is not an uncompressed RGBA source");
        return false;
    }
    out.width = image.width;
    out.height = image.height;
    out.layers = std::max(1u, image.layer_count) * image.face_count;
    const std::vector<u8>& level = image.levels[0];
    const usize values = static_cast<usize>(image.width) * image.height * out.layers * 4u;
    if (info->type_size == 1u) {
        out.rgba8 = level;
    } else if (info->type_size == 2u) {
        out.rgba16f.resize(values);
        for (usize i = 0; i < values; ++i) {
            out.rgba16f[i] = static_cast<u16>(level[i * 2u] | (level[i * 2u + 1u] << 8));
        }
    } else {
        out.rgba16f.resize(values);
        for (usize i = 0; i < values; ++i) {
            const u32 bits = get_u32(level.data() + i * 4u);
            f32 value = 0.f;
            std::memcpy(&value, &bits, sizeof(value));
            out.rgba16f[i] = float_to_half(value);
        }
    }
    return true;
}

bool texture_source_to_ktx2(const TextureSource& source, bool srgb, bool cube, Ktx2Image& out, std::string* error) {
    out = Ktx2Image{};
    const usize values = static_cast<usize>(source.width) * source.height * source.layers * 4u;
    if (source.width == 0u || source.height == 0u || source.layers == 0u || (cube && source.layers % 6u != 0u)) {
        set_error(error, "invalid texture source layout");
        return false;
    }
    out.width = source.width;
    out.height = source.height;
    out.face_count = cube ? 6u : 1u;
    const u32 arrayLayers = cube ? source.layers / 6u : source.layers;
    out.layer_count = arrayLayers > 1u ? arrayLayers : 0u;
    if (source.rgba8.size() == values) {
        out.vk_format = srgb ? vk_format::kR8G8B8A8Srgb : vk_format::kR8G8B8A8Unorm;
        out.levels.push_back(source.rgba8);
    } else if (source.rgba16f.size() == values) {
        out.vk_format = vk_format::kR16G16B16A16Sfloat;
        std::vector<u8> bytes;
        bytes.reserve(values * 2u);
        for (u16 v : source.rgba16f) {
            bytes.push_back(static_cast<u8>(v & 0xFFu));
            bytes.push_back(static_cast<u8>(v >> 8));
        }
        out.levels.push_back(std::move(bytes));
    } else {
        set_error(error, "texture source pixels missing");
        return false;
    }
    return true;
}

} // namespace fuse::cook
