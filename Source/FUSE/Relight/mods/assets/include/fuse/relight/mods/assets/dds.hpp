// FUSE Relight RL-3.3: DDS reader for mod textures (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// Covers the §4.6 superset of what Remix mods ship (upstream loads them through gli in
// rtx_asset_data_manager.cpp; this is FUSE's own reader, no gli):
//   - the classic DX9 header (DDS_HEADER + DDS_PIXELFORMAT) with FourCC formats (DXT1-5, ATI1/ATI2,
//     BC4U/BC4S/BC5U/BC5S, numeric D3DFORMAT codes for the float and 16-bit formats) and bit-mask formats
//     (the D3DX mask table, the same one capture/export's writer emits);
//   - the DX10 extension header (DXGI_FORMAT, resource dimension, cube flag, array size, alpha mode);
//   - BC1-BC7 including BC6H (UF16/SF16), R8G8B8A8 / B8G8R8A8 (+ sRGB), R16F, R32F and the other DXGI
//     colour formats in TexFormat; mip chains, arrays, cube maps (DX9 cubes need all six faces) and
//     volumes.
// BCn blocks are returned exactly as stored (uploaded without re-encoding). DX9 formats without a Vulkan
// twin either keep their bytes with a component swizzle (L8, A8L8, L16, A8, X8R8G8B8, X1R5G5B5, ...) or
// are expanded to R8G8B8A8_UNORM at load (R8G8B8, R3G3B2, A8R3G3B2, A4R4G4B4, X4R4G4B4, A4L4;
// DdsInfo::converted). Every size is validated against the file before anything is allocated; malformed
// input returns nullopt with a reason (fuzzed by rl_mods_assets_fuzz).
//
// Sharing with RL-1.8: capture/export/dds.* writes the DX9 files of captured textures in Remix's
// canonical hash layout. This reader is the superset the runtime needs; the RL-3.3 tests read every file
// that writer produces (all texture_formats D3DFORMATs) and check the bytes and the format mapping.
#pragma once

#include <fuse/relight/hash/d3d_types.hpp>
#include <fuse/relight/mods/assets/texture_format.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fuse::relight::mods::assets {

struct DdsInfo {
    bool dx10Header = false;
    std::uint32_t dxgiFormat = 0;                          ///< DX10 header only
    hash::D3DFormat legacyFormat = hash::D3DFormat::Unknown; ///< DX9 header only
    bool converted = false; ///< texels were expanded to R8G8B8A8_UNORM (no Vulkan twin)
    std::uint64_t trailingBytes = 0;
};

struct DdsTexture {
    TextureImage image;
    DdsInfo info;
};

/// Limits applied before any allocation (larger headers are rejected as malformed).
inline constexpr std::uint32_t kDdsMaxExtent = 32768;
inline constexpr std::uint32_t kDdsMaxDepth = 2048;
inline constexpr std::uint32_t kDdsMaxArraySize = 2048;

std::optional<DdsTexture> readDds(std::span<const std::uint8_t> file, std::string* error = nullptr);
std::optional<DdsTexture> readDdsFile(const std::filesystem::path& path, std::string* error = nullptr);

/// The Vulkan-side format of a DXGI_FORMAT value (TYPELESS BCn / RGBA8 / BGRA8 map to UNORM), with the
/// swizzle for A8_UNORM and the X formats; Undefined when unsupported.
TexFormat texFormatFromDxgi(std::uint32_t dxgiFormat, Swizzle* swizzle = nullptr);
/// Inverse used by writeDds (0 when the format has no DXGI spelling).
std::uint32_t dxgiFromTexFormat(TexFormat format);

/// Writes `image` with a DX10 header (tools and tests; the swizzle must be identity). Empty with `error`
/// set when the format has no DXGI spelling or the data does not match the layout.
std::vector<std::uint8_t> writeDds(const TextureImage& image, std::string* error = nullptr);

} // namespace fuse::relight::mods::assets
