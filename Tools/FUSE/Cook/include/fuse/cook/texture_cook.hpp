#pragma once

#include <fuse/cook/bcn_encoder.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

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
};

/// Largest width or height a texture cook accepts (the common D3D12 / Vulkan 2D image limit). Larger
/// sources are rejected with `CookFailure::InvalidImageDimensions` before any pixels are decoded.
inline constexpr u32 kMaxCookTextureDimension = 16384u;
/// Largest array layer count (Vulkan's guaranteed minimum for maxImageArrayLayers).
inline constexpr u32 kMaxCookTextureLayers = 2048u;

/// How a texture is cooked (asset plan §1.3 format table).
struct TextureCookOptions {
    BcFormat format = BcFormat::BC7;
    bool srgb = true;        ///< honoured for BC1 / BC7; BC4 / BC5 / BC6H are always linear
    bool normal_map = false; ///< forces BC5 + linear; source RGB decoded as a unit vector, mips renormalised
    bool mipmaps = true;
    bool cube = false;       ///< source layers are cube faces (layer count must be a multiple of 6)
    f32 texel_m = 0.f;       ///< recorded in the header when > 0
};

/// Uncompressed source pixels for a cook: `layers` images of `width`×`height`, stacked layer after
/// layer, as RGBA8 (`rgba8`) or RGBA half floats (`rgba16f`, HDR sources). BC6H accepts either (LDR
/// sources are taken as linear [0, 1]); the LDR formats need `rgba8`.
struct TextureSource {
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;
    std::vector<u8> rgba8;
    std::vector<u16> rgba16f;
};

/// Build the mip chain and encode every level/layer. Fails (`InvalidImageDimensions`,
/// `InvalidArgument`) without touching `out` on invalid input.
CookStubWriteResult cook_texture_image(const TextureSource& source, const TextureCookOptions& options,
                                       CookedTexture& out);

/// Serialize / parse the `.fusetex` container (see `CookedTexture`).
std::vector<u8> serialize_cooked_texture(const CookedTexture& texture);
bool parse_cooked_texture(const u8* data, usize size, CookedTexture& out, std::string* error = nullptr);

/// Write a cooked texture: `.ktx2` output paths get a KTX2 container (transport), anything else a
/// `.fusetex`.
CookStubWriteResult write_cooked_texture(const CookedTexture& texture, const std::string& output_path);

/// Decode a source file into pixels: PNG / TGA / BMP / JPEG (stb_image, RGBA8), Radiance `.hdr`
/// (stb_image, RGBA16F when `keep_hdr`, otherwise tone-mapped to RGBA8 by stb_image) or an
/// uncompressed `.ktx2` (RGBA8 / RGBA16F / RGBA32F, level 0, all layers).
bool load_texture_source(const std::string& input_path, TextureSource& out, CookFailure* failure = nullptr,
                         std::string* error = nullptr, bool keep_hdr = true);

/// Full cook: decode `input_path` (see `load_texture_source`; a block-compressed `.ktx2` or a
/// `.fusetex` is passed through without re-encoding when its format matches `options.format`), encode,
/// and write `output_path` (`.ktx2` → KTX2 export, otherwise `.fusetex`).
CookStubWriteResult cook_texture_file(const std::string& input_path, const std::string& output_path,
                                      const TextureCookOptions& options);

/// Decode an image (PNG / TGA / BMP / JPEG via stb_image), build mips when requested, BC7-encode and
/// write. Fails without writing when the source cannot be decoded (`CorruptImage`), is zero-size or
/// larger than `kMaxCookTextureDimension` (`InvalidImageDimensions`), or stb_image is not linked
/// (`ImporterUnavailable`). Non-power-of-two and non-multiple-of-4 sizes are valid: BC7 blocks are
/// edge-padded and each mip level halves (floor, min 1), so there is no POT requirement to enforce.
CookStubWriteResult cook_texture_bc7_file(const std::string& input_path, const std::string& output_path,
                                          bool mipmaps);

/// Write an in-memory RGBA8 image as a cooked BC7 texture (same format as `cook_texture_bc7_file`).
CookStubWriteResult write_texture_bc7_rgba(const u8* rgba, u32 width, u32 height, const std::string& output_path,
                                           bool mipmaps, const char* hook = "bc7_mode6");

/// Parse and validate a cooked texture file (`.fusetex`, or a BCn `.ktx2`).
bool load_cooked_texture(const std::string& path, CookedTexture& out, std::string* error = nullptr);

} // namespace fuse::cook
