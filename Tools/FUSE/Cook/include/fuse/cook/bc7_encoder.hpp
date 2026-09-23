#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

/// BC7 block encoder — spec-conformant modes 6 (one RGBA subset, 7.7.7.7+p, 4-bit indices),
/// 1 (two opaque RGB subsets, 6.6.6+shared p, 3-bit indices) and 7 (two RGBA subsets, 5.5.5.5+p,
/// 2-bit indices). Every block tries mode 6; blocks with high mode-6 error also try the six most
/// promising two-subset partitions. Endpoints come from each subset's principal axis, refined by
/// least squares, with an exhaustive p-bit search. Blocks decode with any BC7 decoder.
struct Bc7EncodeResult {
    bool ok = false;
    u32 width = 0;
    u32 height = 0;
    u32 blockCount = 0;
    u32 byteCount = 0;
    std::string note;
};

struct Bc7RgbaImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

/// Encode one 4×4 RGBA8 tile (row-major, 64 bytes) into a 16-byte BC7 block (mode 1, 6 or 7).
void bc7_encode_block(const u8* rgba4x4, u8 block[16]);

/// Decode a BC7 block into 16 RGBA8 texels. Supports the modes the encoder emits (1, 6, 7);
/// returns false for any other mode.
bool bc7_decode_block(const u8 block[16], u8 rgba4x4[64]);

/// Encode one solid-colour block (decodes within ±1 per channel).
void bc7_encode_solid_block(u8 block[16], u8 r, u8 g, u8 b, u8 a = 255);

/// Encode one 4×4 RGBA8 tile — alias of `bc7_encode_block`.
void bc7_encode_dual_endpoint_block(const u8* rgba4x4, u8 block[16]);

/// Decode a block and return texel 0 (solid-block convenience).
bool bc7_decode_solid_block(const u8 block[16], u8& r, u8& g, u8& b, u8& a);

/// Decode a block — alias of `bc7_decode_block`.
bool bc7_decode_dual_endpoint_block(const u8 block[16], u8 rgba4x4[64]);

/// Pad dimensions up to multiples of 4.
u32 bc7_padded_dimension(u32 value);

/// Deterministic RGBA synthesis from arbitrary source bytes (legacy lenient-cook placeholder).
Bc7RgbaImage synthesize_rgba_from_source(const std::string& source_path);

/// Encode RGBA8 image into BC7 blocks (edge texels clamped into padding).
Bc7EncodeResult encode_bc7_rgba8(const u8* rgba, u32 width, u32 height, std::vector<u8>& outBlocks);

/// Decode BC7 blocks back into an RGBA8 image of `width`×`height` (padding cropped).
bool decode_bc7_rgba8(const u8* blocks, usize blockBytes, u32 width, u32 height, std::vector<u8>& outRgba);

/// Box-filter mip chain (level 0 is `rgba`), down to 1×1. Deterministic integer rounding.
std::vector<Bc7RgbaImage> build_rgba_mip_chain(const u8* rgba, u32 width, u32 height);

} // namespace fuse::cook
