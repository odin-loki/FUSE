// FUSE Relight: Remix-compatible texture, material and render-target descriptor hashes
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.3-4.1.4).
//
// Upstream (dxvk-remix @0867d3c, D3D9CommonTexture::SetupForRtxFrom): a D3DRTYPE_TEXTURE without
// D3DUSAGE_DEPTHSTENCIL is hashed once, at the first upload of subresource 0, as
// XXH3_64bits(buffer, size) over the mip-0 staging buffer (XXH64(buffer, size, 0) with
// rtx.useObsoleteHashOnTextureUpload). That buffer has DXVK's packed layout (GetMipSize):
//     size = min(planeCount, 2) * align(elementSize * blocksWide, 4) * blocksHigh * depth
// where elementSize, the block extent and planeCount come from the D3DFORMAT -> VkFormat mapping
// (d3d9_format.cpp, with the device fix-ups of D3D9VkFormatTable::GetFormatMapping) or, for
// formats without a mapping, D3D9VkFormatTable::GetUnsupportedFormatInfo.
// Relight re-packs the rows the application wrote into this canonical layout (packTextureMip0),
// so the hash never depends on the vendored DXVK's pitch or allocator.
#pragma once

#include <fuse/relight/hash/d3d_types.hpp>
#include <fuse/relight/hash/xxh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuse::relight::hash {

/// The device/option inputs of D3D9VkFormatTable::GetFormatMapping that change a format's layout.
/// Defaults are DXVK's option defaults on an NVIDIA device.
struct FormatTableOptions {
    bool supportX4R4G4B4 = true; // d3d9.supportX4R4G4B4
    bool supportDfFormats = true; // d3d9.supportDFFormats
    bool supportD32 = true;       // d3d9.supportD32
    bool d24s8Supported = true;   // VK_FORMAT_D24_UNORM_S8_UINT usable (else D32_SFLOAT_S8_UINT)
};

/// What GetMipSize reads for one D3D format.
struct TextureFormatInfo {
    bool mapped = false;         // ConvertFormatUnfixed/GetFormatMapping gave a VkFormat
    std::uint32_t vkFormat = 0;  // mapping.FormatColor (vk_format::kUndefined when unmapped)
    std::uint32_t elementSize = 0; // bytes per texel / block (0 for unknown formats)
    std::uint32_t blockWidth = 1;
    std::uint32_t blockHeight = 1;
    std::uint32_t blockDepth = 1;
    std::uint32_t planeCount = 1; // ConversionFormatInfo.PlaneCount (NV12 = 2, YV12 = 3)
};

[[nodiscard]] TextureFormatInfo textureFormatInfo(D3DFormat format, const FormatTableOptions& options = {}) noexcept;

/// The canonical mip-0 layout.
struct TextureMip0Layout {
    std::uint32_t blocksWide = 0;
    std::uint32_t blocksHigh = 0;
    std::uint32_t depth = 0;
    std::uint32_t planes = 0;      // min(planeCount, 2)
    std::uint64_t rowBytes = 0;    // align(elementSize * blocksWide, 4)
    std::uint64_t rowCount = 0;    // planes * blocksHigh * depth
    std::uint64_t size = 0;        // rowBytes * rowCount

    friend bool operator==(const TextureMip0Layout&, const TextureMip0Layout&) = default;
};

[[nodiscard]] TextureMip0Layout textureMip0Layout(D3DFormat format, std::uint32_t width, std::uint32_t height,
                                                  std::uint32_t depth = 1, const FormatTableOptions& options = {}) noexcept;

/// Re-packs rows into the canonical layout: row r (0 <= r < layout.rowCount) is read from
/// src + r * srcRowPitch; min(srcRowPitch, rowBytes) bytes are copied and the rest zero-filled.
[[nodiscard]] std::vector<std::uint8_t> packTextureMip0(const TextureMip0Layout& layout, const void* src,
                                                        std::size_t srcRowPitch);

/// True when upstream hashes the texture at all: D3DRTYPE_TEXTURE without D3DUSAGE_DEPTHSTENCIL.
[[nodiscard]] bool isTextureHashed(D3DResourceType type, std::uint32_t usage) noexcept;

/// The texture ("remix.tex") hash: XXH3_64bits over the canonical mip-0 bytes.
[[nodiscard]] Hash64 hashTextureMip0(const void* packed, std::size_t size) noexcept;

/// rtx.useObsoleteHashOnTextureUpload ("remix.tex.obsolete"): XXH64(data, size, 0).
[[nodiscard]] Hash64 hashTextureMip0Obsolete(const void* packed, std::size_t size) noexcept;

/// LegacyMaterialData::updateCachedHash: the material hash is the stage-0 colour texture's hash
/// (0 for untextured draws, which therefore cannot be material-replaced).
[[nodiscard]] constexpr Hash64 legacyMaterialHash(Hash64 colorTexture0Hash) noexcept {
    return colorTexture0Hash;
}

/// D3D9_COMMON_TEXTURE_DESC (44 bytes, NV-DXVK layout), after NormalizeTextureProperties.
struct TextureDescriptor {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t arraySize = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t usage = 0;
    std::uint32_t format = 0; // D3DFORMAT
    std::uint32_t pool = 0;   // D3DPOOL
    std::uint32_t multiSample = 0;
    std::uint32_t multisampleQuality = 0;
    bool discard = false;
    bool isBackBuffer = false;
    bool isAttachmentOnly = false;
};

inline constexpr std::size_t kTextureDescriptorBytes = 44;

/// The exact bytes D3D9_COMMON_TEXTURE_DESC::CalculateHash hashes (padding byte = 0).
[[nodiscard]] std::array<std::uint8_t, kTextureDescriptorBytes> serializeTextureDescriptor(const TextureDescriptor& desc) noexcept;

/// The render-target descriptor hash ("remix.rtdesc"): XXH3_64bits over the 44 descriptor bytes.
[[nodiscard]] Hash64 hashTextureDescriptor(const TextureDescriptor& desc) noexcept;

/// util::computeMipLevelCount({w, h, d}): what NormalizeTextureProperties stores for MipLevels 0.
[[nodiscard]] std::uint32_t fullMipLevelCount(std::uint32_t width, std::uint32_t height, std::uint32_t depth = 1) noexcept;

} // namespace fuse::relight::hash
