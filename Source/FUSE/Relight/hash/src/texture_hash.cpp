// FUSE Relight: Remix-compatible texture and descriptor hashes.
//
// The D3DFORMAT layout table restates, as hash-only data, DXVK's ConvertFormatUnfixed,
// D3D9VkFormatTable::GetFormatMapping / GetUnsupportedFormatInfo (src/d3d9/d3d9_format.cpp) and the
// element and block sizes of imageFormatInfo (src/dxvk/dxvk_format.cpp), as they are in
// dxvk-remix @0867d3c. DXVK is under the zlib licence:
//
//     Copyright (c) 2017-2021 Philip Rebohle
//     Copyright (c) 2019-2021 Joshua Ashton
//
//     This software is provided 'as-is', without any express or implied warranty. In no event
//     will the authors be held liable for any damages arising from the use of this software.
//     Permission is granted to anyone to use this software for any purpose, including commercial
//     applications, and to alter it and redistribute it freely, subject to the following
//     restrictions:
//     1. The origin of this software must not be misrepresented; you must not claim that you
//        wrote the original software. If you use this software in a product, an acknowledgment in
//        the product documentation would be appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//        being the original software.
//     3. This notice may not be removed or altered from any source distribution.
//
// Altered source (zlib clause 2): FUSE Relight keeps only the layout-relevant columns (VkFormat,
// element size, block extent, plane count) in its own table; swizzles, aspects and sRGB variants
// are dropped. GetMipSize (d3d9_common_texture.cpp) and D3D9_COMMON_TEXTURE_DESC::CalculateHash
// (NV-DXVK, MIT, Copyright (c) NVIDIA CORPORATION) are restated below.
#include <fuse/relight/hash/texture_hash.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::hash {

namespace {

struct VkFormatLayout {
    std::uint32_t elementSize;
    std::uint32_t blockWidth;
    std::uint32_t blockHeight;
};

// imageFormatInfo(vkFormat): element size and block extent of every VkFormat the table uses.
VkFormatLayout vkFormatLayout(std::uint32_t vkFormat) noexcept {
    namespace vf = vk_format;
    switch (vkFormat) {
    case vf::kR4G4UnormPack8:
    case vf::kR8Unorm:
    case vf::kR8Uint:
    case vf::kR8Srgb:
    case vf::kS8Uint:
        return {1, 1, 1};
    case vf::kB4G4R4A4UnormPack16:
    case vf::kR5G6B5UnormPack16:
    case vf::kB5G6R5UnormPack16:
    case vf::kA1R5G5B5UnormPack16:
    case vf::kA4R4G4B4UnormPack16:
    case vf::kR8G8Unorm:
    case vf::kR8G8Snorm:
    case vf::kR16Unorm:
    case vf::kR16Uint:
    case vf::kR16Sfloat:
    case vf::kD16Unorm:
        return {2, 1, 1};
    case vf::kR8G8B8A8Unorm:
    case vf::kR8G8B8A8Snorm:
    case vf::kR8G8B8A8Srgb:
    case vf::kB8G8R8A8Unorm:
    case vf::kB8G8R8A8Srgb:
    case vf::kA2R10G10B10UnormPack32:
    case vf::kA2B10G10R10UnormPack32:
    case vf::kA2B10G10R10SnormPack32:
    case vf::kR16G16Unorm:
    case vf::kR16G16Snorm:
    case vf::kR16G16Sfloat:
    case vf::kR32Uint:
    case vf::kR32Sfloat:
    case vf::kB10G11R11UfloatPack32:
    case vf::kD32Sfloat:
    case vf::kD24UnormS8Uint:
        return {4, 1, 1};
    case vf::kR16G16B16A16Unorm:
    case vf::kR16G16B16A16Snorm:
    case vf::kR16G16B16A16Sfloat:
    case vf::kR32G32Sfloat:
    case vf::kD32SfloatS8Uint:
        return {8, 1, 1};
    case vf::kR32G32B32A32Sfloat:
        return {16, 1, 1};
    case vf::kBc1RgbaUnormBlock:
    case vf::kBc4UnormBlock:
        return {8, 4, 4};
    case vf::kBc2UnormBlock:
    case vf::kBc3UnormBlock:
    case vf::kBc5UnormBlock:
        return {16, 4, 4};
    case vf::kG8B8G8R8422Unorm:
    case vf::kB8G8R8G8422Unorm:
        return {4, 2, 1};
    default:
        return {0, 1, 1};
    }
}

struct FormatMapping {
    std::uint32_t vkFormat;
    std::uint32_t planeCount;
};

// ConvertFormatUnfixed: FormatColor and ConversionFormatInfo.PlaneCount ({0, 1} = no mapping).
FormatMapping convertFormatUnfixed(D3DFormat format) noexcept {
    namespace vf = vk_format;
    using F = D3DFormat;
    switch (format) {
    case F::A8R8G8B8:
    case F::X8R8G8B8: return {vf::kB8G8R8A8Unorm, 1};
    case F::R5G6B5: return {vf::kR5G6B5UnormPack16, 1};
    case F::X1R5G5B5:
    case F::A1R5G5B5: return {vf::kA1R5G5B5UnormPack16, 1};
    case F::A4R4G4B4:
    case F::X4R4G4B4: return {vf::kA4R4G4B4UnormPack16, 1};
    case F::A8: return {vf::kR8Unorm, 1};
    case F::A2B10G10R10: return {vf::kA2B10G10R10UnormPack32, 1};
    case F::A8B8G8R8:
    case F::X8B8G8R8: return {vf::kR8G8B8A8Unorm, 1};
    case F::G16R16: return {vf::kR16G16Unorm, 1};
    case F::A2R10G10B10: return {vf::kA2R10G10B10UnormPack32, 1};
    case F::A16B16G16R16: return {vf::kR16G16B16A16Unorm, 1};
    case F::L8: return {vf::kR8Unorm, 1};
    case F::A8L8: return {vf::kR8G8Unorm, 1};
    case F::A4L4: return {vf::kR4G4UnormPack8, 1};
    case F::V8U8: return {vf::kR8G8Snorm, 1};
    case F::L6V5U5: return {vf::kB5G6R5UnormPack16, 1};
    case F::X8L8V8U8: return {vf::kB8G8R8A8Unorm, 1};
    case F::Q8W8V8U8: return {vf::kR8G8B8A8Snorm, 1};
    case F::V16U16: return {vf::kR16G16Snorm, 1};
    case F::A2W10V10U10: return {vf::kA2B10G10R10UnormPack32, 1};
    case F::W11V11U10: return {vf::kB10G11R11UfloatPack32, 1};
    case F::UYVY:
    case F::YUY2: return {vf::kB8G8R8A8Unorm, 1};
    case F::R8G8_B8G8: return {vf::kG8B8G8R8422Unorm, 1};
    case F::G8R8_G8B8: return {vf::kB8G8R8G8422Unorm, 1};
    case F::DXT1: return {vf::kBc1RgbaUnormBlock, 1};
    case F::DXT2:
    case F::DXT3: return {vf::kBc2UnormBlock, 1};
    case F::DXT4:
    case F::DXT5: return {vf::kBc3UnormBlock, 1};
    case F::D16_LOCKABLE:
    case F::D16: return {vf::kD16Unorm, 1};
    case F::D32:
    case F::D32F_LOCKABLE:
    case F::D32_LOCKABLE: return {vf::kD32Sfloat, 1};
    case F::D24S8:
    case F::D24X8:
    case F::D24FS8: return {vf::kD24UnormS8Uint, 1};
    case F::S8_LOCKABLE: return {vf::kS8Uint, 1};
    case F::L16: return {vf::kR16Unorm, 1};
    case F::VERTEXDATA: return {vf::kR8Uint, 1};
    case F::INDEX16: return {vf::kR16Uint, 1};
    case F::INDEX32: return {vf::kR32Uint, 1};
    case F::Q16W16V16U16: return {vf::kR16G16B16A16Snorm, 1};
    case F::R16F: return {vf::kR16Sfloat, 1};
    case F::G16R16F: return {vf::kR16G16Sfloat, 1};
    case F::A16B16G16R16F: return {vf::kR16G16B16A16Sfloat, 1};
    case F::R32F: return {vf::kR32Sfloat, 1};
    case F::G32R32F: return {vf::kR32G32Sfloat, 1};
    case F::A32B32G32R32F: return {vf::kR32G32B32A32Sfloat, 1};
    case F::A2B10G10R10_XR_BIAS: return {vf::kA2B10G10R10SnormPack32, 1};
    case F::BINARYBUFFER: return {vf::kR8Uint, 1};
    case F::ATI1: return {vf::kBc4UnormBlock, 1};
    case F::ATI2: return {vf::kBc5UnormBlock, 1};
    case F::DF24: return {vf::kD24UnormS8Uint, 1};
    case F::DF16: return {vf::kD16Unorm, 1};
    case F::INTZ: return {vf::kD24UnormS8Uint, 1};
    case F::NV12: return {vf::kR8Unorm, 2};
    case F::YV12: return {vf::kR8Unorm, 3};
    default: return {vf::kUndefined, 1}; // Unsupported, driver hack or unknown
    }
}

// D3D9VkFormatTable::GetUnsupportedFormatInfo element sizes (formats without a mapping).
std::uint32_t unsupportedFormatElementSize(D3DFormat format) noexcept {
    switch (format) {
    case D3DFormat::R8G8B8: return 3;
    case D3DFormat::R3G3B2: return 1;
    case D3DFormat::A8R3G3B2: return 2;
    case D3DFormat::A8P8: return 2;
    case D3DFormat::P8: return 1;
    case D3DFormat::L6V5U5: return 2;
    case D3DFormat::X8L8V8U8: return 4;
    case D3DFormat::A2W10V10U10: return 4;
    case D3DFormat::CxV8U8: return 2;
    default: return 0;
    }
}

std::uint64_t align4(std::uint64_t v) noexcept {
    return (v + 3u) & ~std::uint64_t(3u);
}

} // namespace

TextureFormatInfo textureFormatInfo(D3DFormat format, const FormatTableOptions& options) noexcept {
    FormatMapping mapping = convertFormatUnfixed(format);
    // GetFormatMapping: option-dependent formats lose their mapping entirely.
    if ((format == D3DFormat::X4R4G4B4 && !options.supportX4R4G4B4) ||
        ((format == D3DFormat::DF16 || format == D3DFormat::DF24) && !options.supportDfFormats) ||
        (format == D3DFormat::D32 && !options.supportD32)) {
        mapping = {vk_format::kUndefined, 1};
    }
    if (!options.d24s8Supported && mapping.vkFormat == vk_format::kD24UnormS8Uint) {
        mapping.vkFormat = vk_format::kD32SfloatS8Uint;
    }
    // (The A4R4G4B4 -> B4G4R4A4 fallback keeps the 2-byte element size, so it is not modelled.)

    TextureFormatInfo info;
    info.planeCount = mapping.planeCount;
    if (mapping.vkFormat != vk_format::kUndefined) {
        const VkFormatLayout layout = vkFormatLayout(mapping.vkFormat);
        info.mapped = true;
        info.vkFormat = mapping.vkFormat;
        info.elementSize = layout.elementSize;
        info.blockWidth = layout.blockWidth;
        info.blockHeight = layout.blockHeight;
    } else {
        info.elementSize = unsupportedFormatElementSize(format);
    }
    return info;
}

TextureMip0Layout textureMip0Layout(D3DFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                    const FormatTableOptions& options) noexcept {
    const TextureFormatInfo info = textureFormatInfo(format, options);
    // util::computeMipLevelExtent(extent, 0) clamps each dimension to >= 1.
    width = std::max(1u, width);
    height = std::max(1u, height);
    depth = std::max(1u, depth);
    TextureMip0Layout l;
    l.blocksWide = (width + info.blockWidth - 1) / info.blockWidth;
    l.blocksHigh = (height + info.blockHeight - 1) / info.blockHeight;
    l.depth = (depth + info.blockDepth - 1) / info.blockDepth;
    l.planes = std::min(info.planeCount, 2u);
    l.rowBytes = align4(std::uint64_t(info.elementSize) * l.blocksWide);
    l.rowCount = std::uint64_t(l.planes) * l.blocksHigh * l.depth;
    l.size = l.rowBytes * l.rowCount;
    return l;
}

std::vector<std::uint8_t> packTextureMip0(const TextureMip0Layout& layout, const void* src, std::size_t srcRowPitch) {
    std::vector<std::uint8_t> out(std::size_t(layout.size), 0);
    const auto* s = static_cast<const std::uint8_t*>(src);
    const std::size_t copyBytes = std::min<std::size_t>(srcRowPitch, std::size_t(layout.rowBytes));
    if (s == nullptr || copyBytes == 0) {
        return out;
    }
    for (std::uint64_t r = 0; r < layout.rowCount; ++r) {
        std::memcpy(out.data() + std::size_t(r * layout.rowBytes), s + std::size_t(r) * srcRowPitch, copyBytes);
    }
    return out;
}

bool isTextureHashed(D3DResourceType type, std::uint32_t usage) noexcept {
    return type == D3DResourceType::Texture && (usage & kD3DUsageDepthStencil) == 0;
}

Hash64 hashTextureMip0(const void* packed, std::size_t size) noexcept {
    return xxh3_64(packed, size);
}

Hash64 hashTextureMip0Obsolete(const void* packed, std::size_t size) noexcept {
    return xxh64(packed, size, 0);
}

std::array<std::uint8_t, kTextureDescriptorBytes> serializeTextureDescriptor(const TextureDescriptor& d) noexcept {
    std::array<std::uint8_t, kTextureDescriptorBytes> out{};
    const std::uint32_t words[10] = {d.width, d.height, d.depth, d.arraySize, d.mipLevels,
                                     d.usage, d.format, d.pool, d.multiSample, d.multisampleQuality};
    for (std::size_t i = 0; i < 10; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            out[i * 4 + b] = std::uint8_t(words[i] >> (8 * b));
        }
    }
    out[40] = d.discard ? 1 : 0;
    out[41] = d.isBackBuffer ? 1 : 0;
    out[42] = d.isAttachmentOnly ? 1 : 0;
    out[43] = 0; // NV-DXVK: explicit zero padding byte for a stable hash
    return out;
}

Hash64 hashTextureDescriptor(const TextureDescriptor& desc) noexcept {
    const auto bytes = serializeTextureDescriptor(desc);
    return xxh3_64(bytes.data(), bytes.size());
}

std::uint32_t fullMipLevelCount(std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept {
    std::uint32_t maxDim = std::max(std::max(width, height), depth);
    std::uint32_t count = 0;
    while (maxDim > 0) {
        ++count;
        maxDim /= 2;
    }
    return count;
}

} // namespace fuse::relight::hash
