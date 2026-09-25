#pragma once

// FUSE asset plan W0.4 (docs/plans/FUSE_ASSET_PLAN.md §1.3): minimal KTX2 reader/writer. KTX2 is a
// *transport* format only — distributed packs arrive as KTX2 and the cook turns them into `.fusetex`,
// and cooked textures can be exported as KTX2. In-house implementation of the Khronos KTX 2.0
// specification for the formats the cook uses; no libktx dependency.
//
// Supported: 2D textures, arrays, cube maps (faceCount 6), any level count, supercompressionScheme 0
// (none), vkFormats R8G8B8A8_{UNORM,SRGB}, R16G16B16A16_SFLOAT, R32G32B32A32_SFLOAT,
// BC1_RGB_{UNORM,SRGB}, BC4_UNORM, BC5_UNORM, BC6H_UFLOAT, BC7_{UNORM,SRGB}. Rejected with a clear
// error: 3D textures, BasisLZ / UASTC (vkFormat 0), Zstandard / ZLIB supercompression, other formats.
#include <fuse/cook/texture_cook.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

namespace vk_format {
inline constexpr u32 kR8G8B8A8Unorm = 37;
inline constexpr u32 kR8G8B8A8Srgb = 43;
inline constexpr u32 kR16G16B16A16Sfloat = 97;
inline constexpr u32 kR32G32B32A32Sfloat = 109;
inline constexpr u32 kBc1RgbUnorm = 131;
inline constexpr u32 kBc1RgbSrgb = 132;
inline constexpr u32 kBc4Unorm = 139;
inline constexpr u32 kBc5Unorm = 141;
inline constexpr u32 kBc6hUfloat = 143;
inline constexpr u32 kBc7Unorm = 145;
inline constexpr u32 kBc7Srgb = 146;
} // namespace vk_format

struct Ktx2Image {
    u32 vk_format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 layer_count = 0; ///< as stored: 0 = not an array
    u32 face_count = 1;  ///< 1 or 6
    /// levels[i] = level i data (all layers, faces), level 0 largest.
    std::vector<std::vector<u8>> levels;
    /// Key/value pairs in file order (the writer emits `KTXwriter`).
    std::vector<std::pair<std::string, std::string>> key_values;
};

[[nodiscard]] bool ktx2_format_supported(u32 vk_format);
[[nodiscard]] bool ktx2_format_is_block_compressed(u32 vk_format);
/// Bytes of one level (all layers and faces) for a supported format.
[[nodiscard]] u64 ktx2_level_bytes(u32 vk_format, u32 width, u32 height, u32 layers_times_faces);

/// Serialize a KTX2 file (identifier, header, level index, DFD, KVD, level data smallest first).
bool write_ktx2(const Ktx2Image& image, std::vector<u8>& out, std::string* error = nullptr);
/// Parse and validate a KTX2 file.
bool read_ktx2(const u8* data, usize size, Ktx2Image& out, std::string* error = nullptr);
bool read_ktx2_file(const std::string& path, Ktx2Image& out, std::string* error = nullptr);

/// Cooked BCn texture ↔ KTX2 (block data copied verbatim; no re-encoding).
bool cooked_texture_to_ktx2(const CookedTexture& texture, Ktx2Image& out, std::string* error = nullptr);
bool ktx2_to_cooked_texture(const Ktx2Image& image, CookedTexture& out, std::string* error = nullptr);
/// Level 0 of an uncompressed KTX2 as a cook source (RGBA8, or RGBA16F for the float formats).
bool ktx2_to_texture_source(const Ktx2Image& image, TextureSource& out, std::string* error = nullptr);
/// Uncompressed RGBA8 / RGBA16F source (level 0 only) → KTX2 (used by tests and tooling).
bool texture_source_to_ktx2(const TextureSource& source, bool srgb, bool cube, Ktx2Image& out,
                            std::string* error = nullptr);

} // namespace fuse::cook
