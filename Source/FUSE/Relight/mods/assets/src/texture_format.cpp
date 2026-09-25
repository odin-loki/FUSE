// FUSE Relight RL-3.3: texture format table, subresource layout and colour-space resolution (see
// texture_format.hpp).
#include <fuse/relight/mods/assets/texture_format.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace fuse::relight::mods::assets {

namespace {

struct Entry {
    TexFormat format;
    TexFormatInfo info;
};

using K = TexelKind;

// name, block w/h, bytes per block, channels, kind, srgb
const Entry kFormats[] = {
    {TexFormat::R5G6B5_UNORM_PACK16, {"R5G6B5_UNORM_PACK16", 1, 1, 2, 3, K::Packed, false}},
    {TexFormat::A1R5G5B5_UNORM_PACK16, {"A1R5G5B5_UNORM_PACK16", 1, 1, 2, 4, K::Packed, false}},
    {TexFormat::R8_UNORM, {"R8_UNORM", 1, 1, 1, 1, K::Unorm, false}},
    {TexFormat::R8_SNORM, {"R8_SNORM", 1, 1, 1, 1, K::Snorm, false}},
    {TexFormat::R8_UINT, {"R8_UINT", 1, 1, 1, 1, K::Uint, false}},
    {TexFormat::R8_SINT, {"R8_SINT", 1, 1, 1, 1, K::Sint, false}},
    {TexFormat::R8G8_UNORM, {"R8G8_UNORM", 1, 1, 2, 2, K::Unorm, false}},
    {TexFormat::R8G8_SNORM, {"R8G8_SNORM", 1, 1, 2, 2, K::Snorm, false}},
    {TexFormat::R8G8_UINT, {"R8G8_UINT", 1, 1, 2, 2, K::Uint, false}},
    {TexFormat::R8G8_SINT, {"R8G8_SINT", 1, 1, 2, 2, K::Sint, false}},
    {TexFormat::R8G8B8A8_UNORM, {"R8G8B8A8_UNORM", 1, 1, 4, 4, K::Unorm, false}},
    {TexFormat::R8G8B8A8_SNORM, {"R8G8B8A8_SNORM", 1, 1, 4, 4, K::Snorm, false}},
    {TexFormat::R8G8B8A8_UINT, {"R8G8B8A8_UINT", 1, 1, 4, 4, K::Uint, false}},
    {TexFormat::R8G8B8A8_SINT, {"R8G8B8A8_SINT", 1, 1, 4, 4, K::Sint, false}},
    {TexFormat::R8G8B8A8_SRGB, {"R8G8B8A8_SRGB", 1, 1, 4, 4, K::Unorm, true}},
    {TexFormat::B8G8R8A8_UNORM, {"B8G8R8A8_UNORM", 1, 1, 4, 4, K::Unorm, false}},
    {TexFormat::B8G8R8A8_SRGB, {"B8G8R8A8_SRGB", 1, 1, 4, 4, K::Unorm, true}},
    {TexFormat::A2R10G10B10_UNORM_PACK32, {"A2R10G10B10_UNORM_PACK32", 1, 1, 4, 4, K::Packed, false}},
    {TexFormat::A2B10G10R10_UNORM_PACK32, {"A2B10G10R10_UNORM_PACK32", 1, 1, 4, 4, K::Packed, false}},
    {TexFormat::A2B10G10R10_UINT_PACK32, {"A2B10G10R10_UINT_PACK32", 1, 1, 4, 4, K::Packed, false}},
    {TexFormat::R16_UNORM, {"R16_UNORM", 1, 1, 2, 1, K::Unorm, false}},
    {TexFormat::R16_SNORM, {"R16_SNORM", 1, 1, 2, 1, K::Snorm, false}},
    {TexFormat::R16_UINT, {"R16_UINT", 1, 1, 2, 1, K::Uint, false}},
    {TexFormat::R16_SINT, {"R16_SINT", 1, 1, 2, 1, K::Sint, false}},
    {TexFormat::R16_SFLOAT, {"R16_SFLOAT", 1, 1, 2, 1, K::Float, false}},
    {TexFormat::R16G16_UNORM, {"R16G16_UNORM", 1, 1, 4, 2, K::Unorm, false}},
    {TexFormat::R16G16_SNORM, {"R16G16_SNORM", 1, 1, 4, 2, K::Snorm, false}},
    {TexFormat::R16G16_UINT, {"R16G16_UINT", 1, 1, 4, 2, K::Uint, false}},
    {TexFormat::R16G16_SINT, {"R16G16_SINT", 1, 1, 4, 2, K::Sint, false}},
    {TexFormat::R16G16_SFLOAT, {"R16G16_SFLOAT", 1, 1, 4, 2, K::Float, false}},
    {TexFormat::R16G16B16A16_UNORM, {"R16G16B16A16_UNORM", 1, 1, 8, 4, K::Unorm, false}},
    {TexFormat::R16G16B16A16_SNORM, {"R16G16B16A16_SNORM", 1, 1, 8, 4, K::Snorm, false}},
    {TexFormat::R16G16B16A16_UINT, {"R16G16B16A16_UINT", 1, 1, 8, 4, K::Uint, false}},
    {TexFormat::R16G16B16A16_SINT, {"R16G16B16A16_SINT", 1, 1, 8, 4, K::Sint, false}},
    {TexFormat::R16G16B16A16_SFLOAT, {"R16G16B16A16_SFLOAT", 1, 1, 8, 4, K::Float, false}},
    {TexFormat::R32_UINT, {"R32_UINT", 1, 1, 4, 1, K::Uint, false}},
    {TexFormat::R32_SINT, {"R32_SINT", 1, 1, 4, 1, K::Sint, false}},
    {TexFormat::R32_SFLOAT, {"R32_SFLOAT", 1, 1, 4, 1, K::Float, false}},
    {TexFormat::R32G32_UINT, {"R32G32_UINT", 1, 1, 8, 2, K::Uint, false}},
    {TexFormat::R32G32_SINT, {"R32G32_SINT", 1, 1, 8, 2, K::Sint, false}},
    {TexFormat::R32G32_SFLOAT, {"R32G32_SFLOAT", 1, 1, 8, 2, K::Float, false}},
    {TexFormat::R32G32B32_UINT, {"R32G32B32_UINT", 1, 1, 12, 3, K::Uint, false}},
    {TexFormat::R32G32B32_SINT, {"R32G32B32_SINT", 1, 1, 12, 3, K::Sint, false}},
    {TexFormat::R32G32B32_SFLOAT, {"R32G32B32_SFLOAT", 1, 1, 12, 3, K::Float, false}},
    {TexFormat::R32G32B32A32_UINT, {"R32G32B32A32_UINT", 1, 1, 16, 4, K::Uint, false}},
    {TexFormat::R32G32B32A32_SINT, {"R32G32B32A32_SINT", 1, 1, 16, 4, K::Sint, false}},
    {TexFormat::R32G32B32A32_SFLOAT, {"R32G32B32A32_SFLOAT", 1, 1, 16, 4, K::Float, false}},
    {TexFormat::B10G11R11_UFLOAT_PACK32, {"B10G11R11_UFLOAT_PACK32", 1, 1, 4, 3, K::Packed, false}},
    {TexFormat::E5B9G9R9_UFLOAT_PACK32, {"E5B9G9R9_UFLOAT_PACK32", 1, 1, 4, 3, K::Packed, false}},
    {TexFormat::BC1_RGB_UNORM_BLOCK, {"BC1_RGB_UNORM_BLOCK", 4, 4, 8, 3, K::Block, false}},
    {TexFormat::BC1_RGB_SRGB_BLOCK, {"BC1_RGB_SRGB_BLOCK", 4, 4, 8, 3, K::Block, true}},
    {TexFormat::BC1_RGBA_UNORM_BLOCK, {"BC1_RGBA_UNORM_BLOCK", 4, 4, 8, 4, K::Block, false}},
    {TexFormat::BC1_RGBA_SRGB_BLOCK, {"BC1_RGBA_SRGB_BLOCK", 4, 4, 8, 4, K::Block, true}},
    {TexFormat::BC2_UNORM_BLOCK, {"BC2_UNORM_BLOCK", 4, 4, 16, 4, K::Block, false}},
    {TexFormat::BC2_SRGB_BLOCK, {"BC2_SRGB_BLOCK", 4, 4, 16, 4, K::Block, true}},
    {TexFormat::BC3_UNORM_BLOCK, {"BC3_UNORM_BLOCK", 4, 4, 16, 4, K::Block, false}},
    {TexFormat::BC3_SRGB_BLOCK, {"BC3_SRGB_BLOCK", 4, 4, 16, 4, K::Block, true}},
    {TexFormat::BC4_UNORM_BLOCK, {"BC4_UNORM_BLOCK", 4, 4, 8, 1, K::Block, false}},
    {TexFormat::BC4_SNORM_BLOCK, {"BC4_SNORM_BLOCK", 4, 4, 8, 1, K::Block, false}},
    {TexFormat::BC5_UNORM_BLOCK, {"BC5_UNORM_BLOCK", 4, 4, 16, 2, K::Block, false}},
    {TexFormat::BC5_SNORM_BLOCK, {"BC5_SNORM_BLOCK", 4, 4, 16, 2, K::Block, false}},
    {TexFormat::BC6H_UFLOAT_BLOCK, {"BC6H_UFLOAT_BLOCK", 4, 4, 16, 3, K::Block, false}},
    {TexFormat::BC6H_SFLOAT_BLOCK, {"BC6H_SFLOAT_BLOCK", 4, 4, 16, 3, K::Block, false}},
    {TexFormat::BC7_UNORM_BLOCK, {"BC7_UNORM_BLOCK", 4, 4, 16, 4, K::Block, false}},
    {TexFormat::BC7_SRGB_BLOCK, {"BC7_SRGB_BLOCK", 4, 4, 16, 4, K::Block, true}},
};

const std::pair<TexFormat, TexFormat> kSrgbPairs[] = {
    {TexFormat::R8G8B8A8_UNORM, TexFormat::R8G8B8A8_SRGB},
    {TexFormat::B8G8R8A8_UNORM, TexFormat::B8G8R8A8_SRGB},
    {TexFormat::BC1_RGB_UNORM_BLOCK, TexFormat::BC1_RGB_SRGB_BLOCK},
    {TexFormat::BC1_RGBA_UNORM_BLOCK, TexFormat::BC1_RGBA_SRGB_BLOCK},
    {TexFormat::BC2_UNORM_BLOCK, TexFormat::BC2_SRGB_BLOCK},
    {TexFormat::BC3_UNORM_BLOCK, TexFormat::BC3_SRGB_BLOCK},
    {TexFormat::BC7_UNORM_BLOCK, TexFormat::BC7_SRGB_BLOCK},
};

bool mulOk(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

const TexFormatInfo* texFormatInfo(TexFormat format) {
    for (const Entry& e : kFormats) {
        if (e.format == format) {
            return &e.info;
        }
    }
    return nullptr;
}

TexFormat srgbVariant(TexFormat format) {
    for (const auto& [lin, srgb] : kSrgbPairs) {
        if (lin == format) {
            return srgb;
        }
    }
    return format;
}

TexFormat linearVariant(TexFormat format) {
    for (const auto& [lin, srgb] : kSrgbPairs) {
        if (srgb == format) {
            return lin;
        }
    }
    return format;
}

bool hasSrgbVariant(TexFormat format) { return srgbVariant(format) != format || linearVariant(format) != format; }

std::uint64_t texRowPitch(TexFormat format, std::uint32_t width) {
    const TexFormatInfo* info = texFormatInfo(format);
    if (!info || width == 0) {
        return 0;
    }
    const std::uint64_t blocksWide = (std::uint64_t(width) + info->blockWidth - 1) / info->blockWidth;
    return blocksWide * info->bytesPerBlock;
}

std::uint64_t texLevelSize(TexFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    const TexFormatInfo* info = texFormatInfo(format);
    if (!info || width == 0 || height == 0 || depth == 0) {
        return 0;
    }
    const std::uint64_t row = texRowPitch(format, width);
    const std::uint64_t blocksHigh = (std::uint64_t(height) + info->blockHeight - 1) / info->blockHeight;
    std::uint64_t slice = 0, total = 0;
    if (!mulOk(row, blocksHigh, slice) || !mulOk(slice, depth, total)) {
        return 0;
    }
    return total;
}

std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    std::uint32_t m = std::max({width, height, depth, 1u});
    std::uint32_t n = 1;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

const Subresource* TextureImage::subresource(std::uint32_t layer, std::uint32_t face, std::uint32_t mip) const {
    if (layer >= arraySize || face >= faces || mip >= mipLevels) {
        return nullptr;
    }
    const std::size_t idx = (std::size_t(layer) * faces + face) * mipLevels + mip;
    return idx < subresources.size() ? &subresources[idx] : nullptr;
}

std::uint64_t layoutSubresources(TextureImage& image) {
    image.subresources.clear();
    if (!texFormatInfo(image.format) || image.width == 0 || image.height == 0 || image.depth == 0 ||
        image.mipLevels == 0 || image.arraySize == 0 || image.faces == 0) {
        return 0;
    }
    // Size of one layer/face chain first, so a bogus array size fails before any per-subresource work.
    std::uint64_t chain = 0;
    for (std::uint32_t m = 0; m < image.mipLevels; ++m) {
        const std::uint64_t s = texLevelSize(image.format, mipExtent(image.width, m), mipExtent(image.height, m),
                                             mipExtent(image.depth, m));
        if (s == 0 || chain + s < chain) {
            return 0;
        }
        chain += s;
    }
    std::uint64_t count = 0, total = 0;
    if (!mulOk(image.arraySize, image.faces, count) || !mulOk(chain, count, total)) {
        return 0;
    }
    image.subresources.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count * image.mipLevels, 1u << 20)));
    std::uint64_t at = 0;
    for (std::uint32_t l = 0; l < image.arraySize; ++l) {
        for (std::uint32_t f = 0; f < image.faces; ++f) {
            for (std::uint32_t m = 0; m < image.mipLevels; ++m) {
                Subresource s;
                s.layer = l;
                s.face = f;
                s.mip = m;
                s.width = mipExtent(image.width, m);
                s.height = mipExtent(image.height, m);
                s.depth = mipExtent(image.depth, m);
                s.offset = at;
                s.size = texLevelSize(image.format, s.width, s.height, s.depth);
                at += s.size;
                image.subresources.push_back(s);
            }
        }
    }
    return total;
}

ColourSpaceHint colourSpaceFromUsd(std::string_view token) {
    const std::string t = lower(token);
    if (t == "srgb") {
        return ColourSpaceHint::Srgb;
    }
    if (t == "raw" || t == "linear" || t == "lin_rec709" || t == "lin_srgb" || t == "data") {
        return ColourSpaceHint::Linear;
    }
    return ColourSpaceHint::Auto;
}

bool remixParameterIsSrgb(std::string_view parameter) {
    std::string p = lower(parameter);
    if (p.rfind("inputs:", 0) == 0) {
        p = p.substr(7);
    }
    return p == "diffuse_texture" || p == "emissive_mask_texture";
}

TexFormat resolveColourSpace(TexFormat format, ColourSpaceHint hint, std::string_view parameter) {
    switch (hint) {
    case ColourSpaceHint::Srgb:
        return srgbVariant(format);
    case ColourSpaceHint::Linear:
        return linearVariant(format);
    case ColourSpaceHint::Auto:
        break;
    }
    if (parameter.empty()) {
        return format;
    }
    return remixParameterIsSrgb(parameter) ? srgbVariant(format) : linearVariant(format);
}

} // namespace fuse::relight::mods::assets
