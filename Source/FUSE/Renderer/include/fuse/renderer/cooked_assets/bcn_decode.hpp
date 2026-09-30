#pragma once

// E06 (AP-RT-COOKED): CPU decoder for the block-compressed formats of cooked `.fusetex` textures
// (fuse/asset/cooked_texture.hpp): BC1, BC4, BC5, BC6H and BC7, every mode of each (Direct3D 11 functional
// specification / Khronos Data Format Specification "BC" chapters). Own code, no third-party source.
//
// It is the fallback of the renderer's texture upload when the device lacks textureCompressionBC
// (cooked_texture_gpu.hpp), the source of the layered-material CPU reference's mip chains of cooked textures
// (material_layers/ml_mips.hpp) and of the texel pool of materials.eval. Output conventions follow what a Vulkan
// implementation returns when sampling the matching VK_FORMAT_BC* (UNORM) image:
//
//   BC1   RGBA8 (VK_FORMAT_BC1_RGB_*: the three-colour mode's index 3 is opaque black, alpha always 255)
//   BC4   RGBA8 = (r, 0, 0, 255)
//   BC5   RGBA8 = (r, g, 0, 255)          (tangent-space normals: Z is reconstructed by the sampler's user)
//   BC6H  RGBA16F half bits = (r, g, b, 1.0) (unsigned UF16 or signed SF16)
//   BC7   RGBA8 (bit-exact: the format defines integer decoding)
//
// Interpolated BC1 / BC4 / BC5 values are defined as real numbers by the specifications; they are rounded to the
// nearest 8-bit value here. Implementations approximate the interpolation (Lavapipe: 6-bit fixed-point weights), so
// sampled values differ by up to 2 / 255 (fuse_cooked_assets_vk_decode measures it); BC7 and BC6H decode
// bit-exactly. sRGB is a property of the image view, not of the blocks: decoded bytes of an sRGB texture stay
// sRGB-encoded.

#include <fuse/asset/cooked_texture.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::cooked_assets {

/// 4 x 4 texels, row-major, RGBA8 (64 bytes).
void bc1_decode_block(const u8 block[8], u8 rgba[64]);
/// 16 unorm8 values, row-major.
void bc4_decode_block(const u8 block[8], u8 values[16]);
/// 4 x 4 texels RGBA8 = (r, g, 0, 255).
void bc5_decode_block(const u8 block[16], u8 rgba[64]);
/// 4 x 4 texels RGBA8; the reserved mode (first byte 0) decodes to all zeros.
void bc7_decode_block(const u8 block[16], u8 rgba[64]);
/// 4 x 4 texels RGBA16F half bits (alpha 1.0); reserved modes decode to zeros (alpha 1.0).
void bc6h_decode_block(const u8 block[16], u16 rgba[64], bool isSigned = false);

/// Bytes per decoded texel: 8 for BC6H (RGBA16F), 4 otherwise (RGBA8).
[[nodiscard]] u32 bc_decoded_texel_bytes(asset::BcFormat format);

/// Decodes `layers` images of one level (`blocks`: layer after layer, each ceil(w / 4) x ceil(h / 4) blocks) into
/// `out` (w x h texels per layer, layer after layer, bc_decoded_texel_bytes each). Padding texels are cropped.
/// False when `blockBytes` is too small for the level.
bool decode_bc_level(asset::BcFormat format, const u8* blocks, usize blockBytes, u32 width, u32 height, u32 layers,
                     u8* out, bool signedBc6h = false);

/// Every level of a cooked texture decoded, packed the way UploadQueue::stageImage takes a chain: mip-major (every
/// layer of mip 0, then mip 1, ...), tightly packed texels of bc_decoded_texel_bytes(texture.format) bytes.
struct DecodedTexture {
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;
    u32 levels = 0;
    u32 texelBytes = 4;
    std::vector<u8> bytes;
    std::vector<usize> levelOffsets; ///< byte offset of each level (all layers) in `bytes`
};

/// False (with `out` cleared) when the texture's levels do not match its header (never for parse_cooked_texture
/// output).
bool decode_cooked_texture(const asset::CookedTexture& texture, DecodedTexture& out);

/// Layer `layer` of level `level` as packed RGBA8 words (r | g << 8 | b << 16 | a << 24, x fastest): the texel
/// format of the layered-material pool (material_layers). BC6H halves are clamped to [0, 1] and rounded to 8 bits
/// (an LDR approximation; HDR textures are not material-slot content). False on a bad level / layer.
bool decode_cooked_level_rgba8(const asset::CookedTexture& texture, u32 level, u32 layer, std::vector<u32>& out);

/// IEEE 754 binary16 -> f32 (every value, subnormals and specials included).
[[nodiscard]] f32 half_bits_to_float(u16 bits);

} // namespace fuse::renderer::cooked_assets
