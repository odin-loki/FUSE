#pragma once

// FUSE asset plan W0.3 (docs/plans/FUSE_ASSET_PLAN.md §1.3, §5.1): block encoders for every runtime
// texture format the cook emits. All encoders are in-house, deterministic (no threads, no timing, no
// host dependence) and emit spec-conformant blocks (Direct3D 11 functional specification):
//
//   BC1   opaque RGB, four-colour mode (c0 > c1); principal-axis endpoints refined by least squares.
//   BC4   one unorm channel; eight-value mode with a small endpoint search, plus the six-value mode
//         (explicit 0 / 255) when a block holds both extremes.
//   BC5   two BC4 channels (R, G) — tangent-space normal maps (Z reconstructed at sample time).
//   BC6H  unsigned half float (UF16), mode 11 (one region, 10.10.10 endpoints, 4-bit indices).
//   BC7   the existing encoder in bc7_encoder.cpp (modes 1, 6, 7).
//
// ispc_texcomp (hook in ispc_texcomp_hook.cpp) is used only when its header is present; the in-house
// encoders are the default and are what the PSNR gates (test_texture_bcn.cpp) measure.
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

enum class BcFormat : u8 { BC1 = 0, BC4, BC5, BC6H, BC7 };

[[nodiscard]] const char* bc_format_name(BcFormat format);
/// Case-insensitive "BC1" / "BC4" / "BC5" / "BC6H" / "BC7".
[[nodiscard]] bool parse_bc_format(const std::string& text, BcFormat& out);
/// 8 for BC1 / BC4, 16 otherwise.
[[nodiscard]] u32 bc_block_bytes(BcFormat format);
/// Blocks covering a `width`×`height` level (dimensions rounded up to multiples of 4).
[[nodiscard]] u32 bc_block_count(u32 width, u32 height);

// Block level (tiles are 4×4 row-major).
void bc1_encode_block(const u8 rgba[64], u8 block[8]);
void bc1_decode_block(const u8 block[8], u8 rgba[64]);
void bc4_encode_block(const u8 values[16], u8 block[8]);
void bc4_decode_block(const u8 block[8], u8 values[16]);
void bc5_encode_block(const u8 red[16], const u8 green[16], u8 block[16]);
void bc5_decode_block(const u8 block[16], u8 red[16], u8 green[16]);
/// `rgb` holds 16 texels × 3 half-float bit patterns. Negative, NaN and infinite inputs are clamped
/// into the UF16 range [0, 65504] before encoding (UF16 cannot store them).
void bc6h_encode_block(const u16 rgb[48], u8 block[16]);
/// Decodes the mode the encoder emits (mode 11); returns false for any other mode.
bool bc6h_decode_block(const u8 block[16], u16 rgb[48]);

/// A source image: RGBA8 (`rgba8`, LDR formats) or RGBA half floats (`rgba16f`, BC6H). Exactly one
/// of the two must be filled for the format being encoded.
struct BcSourceImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba8;
    std::vector<u16> rgba16f;
};

/// Encode a whole level; edge texels are clamped into the block padding. BC1/BC7 read RGB(A), BC4
/// reads R, BC5 reads R and G, BC6H reads the RGB halves. Returns false on a missing / short source.
bool encode_bc_image(BcFormat format, const BcSourceImage& image, std::vector<u8>& outBlocks,
                     std::string* error = nullptr);

/// Decode a level back to RGBA8 (BC1/BC4/BC5/BC7; missing channels decode as G = B = 0, A = 255)
/// or to RGBA half floats (BC6H, A = 1.0). Padding texels are cropped.
bool decode_bc_image(BcFormat format, const u8* blocks, usize blockBytes, u32 width, u32 height,
                     BcSourceImage& out);

// Half-float helpers (IEEE 754 binary16, round to nearest even).
[[nodiscard]] u16 float_to_half(f32 value);
[[nodiscard]] f32 half_to_float(u16 value);

} // namespace fuse::cook
