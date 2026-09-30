#pragma once

#include <fuse/cook/bcn_encoder.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

// CookedTexture (the `.fusetex` container description), its limits and parse_cooked_texture live in the
// runtime asset library (fuse/asset/cooked_texture.hpp, UNI-U7-ASSET-1); re-exported here.
using asset::CookedTexture;
using asset::kMaxCookTextureDimension;
using asset::kMaxCookTextureLayers;
using asset::parse_cooked_texture;

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

/// Serialize the `.fusetex` container (see `CookedTexture`; parse_cooked_texture is fuse::asset's).
std::vector<u8> serialize_cooked_texture(const CookedTexture& texture);

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
