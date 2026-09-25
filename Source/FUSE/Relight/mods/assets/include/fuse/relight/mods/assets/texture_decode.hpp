// FUSE Relight RL-3.3: CPU decode of every TexFormat to RGBA32F (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// The runtime uploads BCn blocks as they are; this decoder is for CPU consumers (the Remaster store's
// canonical key, thumbnails, import checks, the RL-3.3 "decode vs expected" gate). It returns the stored
// values: sRGB formats are not linearised and the image swizzle is applied only by applySwizzle.
//
// Block formats follow the Direct3D 11 functional specification:
//   BC1-BC3  8-bit integer palettes with the same arithmetic as capture/export's decodeRgba8 (RL-1.8),
//            so both decoders agree bit for bit (checked by the tests);
//   BC4/BC5  float interpolation of the normalised endpoints (unorm /255, snorm /127 clamped to -1);
//   BC6H     all 14 modes, unsigned and signed, to half floats (reserved modes decode to 0);
//   BC7      all 8 modes (a zero mode byte decodes to transparent black).
// The BC6H mode bit layouts and the BC6H/BC7 partition and anchor tables are the specification's tables;
// expected outputs in the tests come from bcdec (Unlicense / MIT, used offline, not vendored).
#pragma once

#include <fuse/relight/mods/assets/texture_format.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fuse::relight::mods::assets {

/// Decodes one level (width x height x depth, tightly packed as in TextureImage) to RGBA32F, row-major,
/// depth slices stacked. Missing channels decode as G = B = 0, A = 1; integer formats give their integer
/// values. nullopt when the format is unknown or `data` is shorter than the level.
std::optional<std::vector<float>> decodeToRgba32f(TexFormat format, std::uint32_t width, std::uint32_t height,
                                                  std::uint32_t depth, std::span<const std::uint8_t> data);
/// Decodes subresource `s` of `image` (the image swizzle is not applied).
std::optional<std::vector<float>> decodeSubresource(const TextureImage& image, const Subresource& s);
/// Applies a component swizzle to RGBA32F texels in place.
void applySwizzle(std::span<float> rgba, Swizzle swizzle);

float halfToFloat(std::uint16_t h);

// Block decoders (16 texels, row-major within the 4x4 block).
void decodeBc1Block(const std::uint8_t* block, std::uint8_t out[16][4], bool forceFourColour);
void decodeBc2Block(const std::uint8_t* block, std::uint8_t out[16][4]);
void decodeBc3Block(const std::uint8_t* block, std::uint8_t out[16][4]);
void decodeBc4Block(const std::uint8_t* block, bool isSigned, float out[16]);
void decodeBc6hBlock(const std::uint8_t* block, bool isSigned, std::uint16_t out[16][3]);
void decodeBc7Block(const std::uint8_t* block, std::uint8_t out[16][4]);

} // namespace fuse::relight::mods::assets
