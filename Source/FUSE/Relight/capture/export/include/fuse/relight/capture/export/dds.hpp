// FUSE Relight RL-1.8: DDS files for captured textures (plan §1.9 "textures exported as DDS", §4.8).
//
// Upstream (dxvk-remix @0867d3c) writes a material's albedo texture to textures/<mat hash>.dds from the
// GPU image (AssetExporter::dumpImageToFile). Relight has no GPU image yet: it writes the application's
// own bytes, which TextureTracker (RL-1.4) keeps in Remix's canonical packed layout (plan §4.1.3). The
// writer therefore never re-encodes: block-compressed formats (DXT1-5, ATI1/2) go through as the
// game's BC blocks ("BC passthrough"), everything else keeps its D3DFORMAT texel layout. Only the row
// pitch changes: canonical rows are align(elementSize * blocksWide, 4) bytes, DDS rows are
// elementSize * blocksWide bytes (the DDS "pitch" rule), so reading a file back and re-packing gives
// the canonical bytes, and XXH3 over them gives the Remix texture hash again (the round trip checked by
// the tests).
//
// Header: the classic DX9 DDS header (DDS_HEADER + DDS_PIXELFORMAT, no DX10 extension), as D3DX writes
// it: RGB / luminance / alpha / bump formats by bit masks, DXTn and ATIn by FourCC, and the formats
// without a mask description (float, 16-bit per channel, Q16W16V16U16, CxV8U8, YUV) by their D3DFORMAT
// value in the FourCC field. `readDds` inverts exactly that mapping.
#pragma once

#include <fuse/relight/hash/d3d_types.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fuse::relight::capture::exporter {

/// One 2D texture in canonical layout: mips[0] is the full-size level, each next level halves the
/// extent (at least 1). Every level's bytes follow hash::textureMip0Layout for its extent.
struct DdsImage {
    hash::D3DFormat format = hash::D3DFormat::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::vector<std::uint8_t>> mips;
};

/// DDS_PIXELFORMAT (32 bytes).
struct DdsPixelFormat {
    std::uint32_t flags = 0;
    std::uint32_t fourCC = 0;
    std::uint32_t rgbBitCount = 0;
    std::uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
};

namespace ddpf {
inline constexpr std::uint32_t AlphaPixels = 0x1, Alpha = 0x2, FourCC = 0x4, Rgb = 0x40, Yuv = 0x200,
                               Luminance = 0x20000, BumpLuminance = 0x40000, BumpDuDv = 0x80000;
}

/// The pixel format D3DX writes for `format`; nullopt when the format has no texel layout the canonical
/// table knows (depth formats, unknown values).
std::optional<DdsPixelFormat> ddsPixelFormat(hash::D3DFormat format);
/// Inverse of ddsPixelFormat (Unknown when no format matches).
hash::D3DFormat d3dFormatFromDds(const DdsPixelFormat& pf);

/// Bytes of one level in the DDS layout (rows of elementSize * blocksWide bytes).
std::uint64_t ddsLevelSize(hash::D3DFormat format, std::uint32_t width, std::uint32_t height);

/// Serialises `image`. Empty with `error` set when the format cannot be written or a level has the
/// wrong size.
std::vector<std::uint8_t> writeDds(const DdsImage& image, std::string* error = nullptr);

/// Parses a file written by writeDds (or any DX9-style 2D DDS whose format maps back); levels are
/// returned in the canonical layout (padding bytes zero).
std::optional<DdsImage> readDds(std::span<const std::uint8_t> file, std::string* error = nullptr);

/// The canonical decoded form the Remaster store keys textures by (plan Remaster §1.3: "RGBA8 mip0"):
/// mip 0 as 8-bit RGBA, row-major, no padding. Implemented for the 8/16/32-bit UNORM colour, luminance
/// and alpha formats and DXT1-5 (DXT2/4 are not un-premultiplied); nullopt for float, signed (bump),
/// YUV and unknown formats, which get no "fuse.capture.sha256" key.
std::optional<std::vector<std::uint8_t>> decodeRgba8(hash::D3DFormat format, std::uint32_t width, std::uint32_t height,
                                                     std::span<const std::uint8_t> canonicalMip0);

} // namespace fuse::relight::capture::exporter
