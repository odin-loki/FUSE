#pragma once

// UNI-U7-ASSET-1: the runtime `.fusetex` reader and the cooked-texture data it produces.
//
// Moved out of the offline cook (Tools/FUSE/Cook, asset plan W0.3) so runtime targets read cooked
// textures without linking stb_image or the BCn encoders. Tools/FUSE/Cook keeps the encoders and the
// KTX2 transport and re-exports these names in `fuse::cook` (fuse/cook/texture_cook.hpp,
// fuse/cook/bcn_encoder.hpp), so existing tools and tests are unchanged.

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::asset {

enum class BcFormat : u8 { BC1 = 0, BC4, BC5, BC6H, BC7 };

[[nodiscard]] const char* bc_format_name(BcFormat format);
/// Case-insensitive "BC1" / "BC4" / "BC5" / "BC6H" / "BC7".
[[nodiscard]] bool parse_bc_format(const std::string& text, BcFormat& out);
/// 8 for BC1 / BC4, 16 otherwise.
[[nodiscard]] u32 bc_block_bytes(BcFormat format);
/// Blocks covering a `width`x`height` level (dimensions rounded up to multiples of 4).
[[nodiscard]] u32 bc_block_count(u32 width, u32 height);
/// Full mip chain length of a `width`x`height` image (down to 1x1).
[[nodiscard]] u32 bc_mip_count(u32 width, u32 height);

/// Cooked block-compressed texture (`.fusetex`): a text header starting `FUSETEX_BC7` (the container
/// magic; kept for existing readers), `DATA\n`, then the blocks of every mip level from largest to
/// 1×1. Within a level the layers follow each other (layer 0 first; a cube map stores faces
/// +X, −X, +Y, −Y, +Z, −Z per array layer). The header never records paths or times, so identical
/// sources cook to identical bytes.
///
/// Header keys: `compression` (BC1 / BC4 / BC5 / BC6H / BC7), `width`, `height`, `blocks` (all levels
/// and layers), `mipmaps`, `mip_levels`; since asset plan W0.3 also `format_version=2`, `layers`,
/// `cube`, `srgb`, `block_bytes`, `normal_convention=gl` (normal maps) and `texel_m` (metres per texel at
/// mip 0, when known). A 2D sRGB BC7 texture keeps the original (version 1) header byte for byte.
struct CookedTexture {
    struct Level {
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> blocks; ///< every layer of this level, layer 0 first
        bool operator==(const Level&) const = default;
    };

    std::string compression; ///< "BC7", "BC5", ...
    BcFormat format = BcFormat::BC7;
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;           ///< array layers × faces
    bool cube = false;        ///< layers are cube faces (layers % 6 == 0)
    bool srgb = true;         ///< colour data stored sRGB-encoded (BC1 / BC7 only)
    bool normal_map = false;  ///< tangent-space normal map (BC5, OpenGL +Y convention)
    f32 texel_m = 0.f;        ///< metres per texel at mip 0 (0: unknown)
    std::vector<Level> levels;
    bool operator==(const CookedTexture&) const = default;
};

/// Largest width or height a texture cook accepts (the common D3D12 / Vulkan 2D image limit). Larger
/// sources are rejected with `CookFailure::InvalidImageDimensions` before any pixels are decoded.
inline constexpr u32 kMaxCookTextureDimension = 16384u;
/// Largest array layer count (Vulkan's guaranteed minimum for maxImageArrayLayers).
inline constexpr u32 kMaxCookTextureLayers = 2048u;

/// Parse and validate a `.fusetex` blob (header keys, format / block size agreement, dimensions,
/// mip chain, block counts). On failure `out` is reset and `error` says why.
bool parse_cooked_texture(const u8* data, usize size, CookedTexture& out, std::string* error = nullptr);

/// Read a `.fusetex` from the host file system and parse it (tools / tests; the runtime reads through
/// the VFS and AssetRegistry). KTX2 is a cook transport format and is read by fuse::cook only.
bool read_cooked_texture_file(const std::string& path, CookedTexture& out, std::string* error = nullptr);

} // namespace fuse::asset
