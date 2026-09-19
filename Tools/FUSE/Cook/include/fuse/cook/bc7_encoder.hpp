#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

/// BC7 block encoder — honest deeper stub (mode 6 solid blocks; not ispc_texcomp).
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

/// Encode one 4×4 RGBA8 block into 16-byte BC7 mode-6 solid payload.
void bc7_encode_solid_block(u8 block[16], u8 r, u8 g, u8 b, u8 a = 255);

/// Decode mode-6 solid block (stub round-trip validation).
bool bc7_decode_solid_block(const u8 block[16], u8& r, u8& g, u8& b, u8& a);

/// Pad dimensions up to multiples of 4.
u32 bc7_padded_dimension(u32 value);

/// Deterministic RGBA synthesis from arbitrary source bytes (no PNG loader yet).
Bc7RgbaImage synthesize_rgba_from_source(const std::string& source_path);

/// Encode RGBA8 image into BC7 blocks (mode 6 per 4×4 tile).
Bc7EncodeResult encode_bc7_rgba8(const u8* rgba, u32 width, u32 height, std::vector<u8>& outBlocks);

} // namespace fuse::cook
