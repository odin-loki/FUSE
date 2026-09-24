// FUSE Relight RL-1.8: DDS writer / reader with BC passthrough, and the RGBA8 decode of the canonical
// texture key (see dds.hpp).
#include <fuse/relight/capture/export/dds.hpp>

#include <fuse/relight/hash/texture_hash.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::exporter {

namespace {

using hash::D3DFormat;

constexpr std::uint32_t kDdsMagic = 0x20534444; // "DDS "
constexpr std::uint32_t kHeaderSize = 124;
constexpr std::uint32_t kPixelFormatSize = 32;
constexpr std::uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                        DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000, DDSD_LINEARSIZE = 0x80000;
constexpr std::uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
constexpr std::uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_VOLUME = 0x200000;

constexpr std::uint32_t fourCC(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) | (std::uint32_t(std::uint8_t(c)) << 16) |
           (std::uint32_t(std::uint8_t(d)) << 24);
}

struct MaskEntry {
    D3DFormat format;
    DdsPixelFormat pf;
};

// D3DX's mask descriptions of the DX9 formats (bit masks are the real channel bits).
const MaskEntry kMaskFormats[] = {
    {D3DFormat::R8G8B8, {ddpf::Rgb, 0, 24, 0xff0000, 0xff00, 0xff, 0}},
    {D3DFormat::A8R8G8B8, {ddpf::Rgb | ddpf::AlphaPixels, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000}},
    {D3DFormat::X8R8G8B8, {ddpf::Rgb, 0, 32, 0xff0000, 0xff00, 0xff, 0}},
    {D3DFormat::R5G6B5, {ddpf::Rgb, 0, 16, 0xf800, 0x7e0, 0x1f, 0}},
    {D3DFormat::X1R5G5B5, {ddpf::Rgb, 0, 16, 0x7c00, 0x3e0, 0x1f, 0}},
    {D3DFormat::A1R5G5B5, {ddpf::Rgb | ddpf::AlphaPixels, 0, 16, 0x7c00, 0x3e0, 0x1f, 0x8000}},
    {D3DFormat::A4R4G4B4, {ddpf::Rgb | ddpf::AlphaPixels, 0, 16, 0xf00, 0xf0, 0xf, 0xf000}},
    {D3DFormat::R3G3B2, {ddpf::Rgb, 0, 8, 0xe0, 0x1c, 0x3, 0}},
    {D3DFormat::A8, {ddpf::Alpha, 0, 8, 0, 0, 0, 0xff}},
    {D3DFormat::A8R3G3B2, {ddpf::Rgb | ddpf::AlphaPixels, 0, 16, 0xe0, 0x1c, 0x3, 0xff00}},
    {D3DFormat::X4R4G4B4, {ddpf::Rgb, 0, 16, 0xf00, 0xf0, 0xf, 0}},
    {D3DFormat::A2B10G10R10, {ddpf::Rgb | ddpf::AlphaPixels, 0, 32, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000}},
    {D3DFormat::A8B8G8R8, {ddpf::Rgb | ddpf::AlphaPixels, 0, 32, 0xff, 0xff00, 0xff0000, 0xff000000}},
    {D3DFormat::X8B8G8R8, {ddpf::Rgb, 0, 32, 0xff, 0xff00, 0xff0000, 0}},
    {D3DFormat::G16R16, {ddpf::Rgb, 0, 32, 0xffff, 0xffff0000, 0, 0}},
    {D3DFormat::A2R10G10B10, {ddpf::Rgb | ddpf::AlphaPixels, 0, 32, 0x3ff00000, 0xffc00, 0x3ff, 0xc0000000}},
    {D3DFormat::L8, {ddpf::Luminance, 0, 8, 0xff, 0, 0, 0}},
    {D3DFormat::A8L8, {ddpf::Luminance | ddpf::AlphaPixels, 0, 16, 0xff, 0, 0, 0xff00}},
    {D3DFormat::A4L4, {ddpf::Luminance | ddpf::AlphaPixels, 0, 8, 0xf, 0, 0, 0xf0}},
    {D3DFormat::L16, {ddpf::Luminance, 0, 16, 0xffff, 0, 0, 0}},
    {D3DFormat::V8U8, {ddpf::BumpDuDv, 0, 16, 0xff, 0xff00, 0, 0}},
    {D3DFormat::L6V5U5, {ddpf::BumpLuminance, 0, 16, 0x1f, 0x3e0, 0xfc00, 0}},
    {D3DFormat::X8L8V8U8, {ddpf::BumpLuminance, 0, 32, 0xff, 0xff00, 0xff0000, 0}},
    {D3DFormat::Q8W8V8U8, {ddpf::BumpDuDv, 0, 32, 0xff, 0xff00, 0xff0000, 0xff000000}},
    {D3DFormat::V16U16, {ddpf::BumpDuDv, 0, 32, 0xffff, 0xffff0000, 0, 0}},
    {D3DFormat::A2W10V10U10, {ddpf::BumpDuDv | ddpf::AlphaPixels, 0, 32, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000}},
};

/// Formats written by FourCC: the named ones use their characters (D3DFORMAT values of these are the
/// same FourCC), the rest their numeric D3DFORMAT value.
bool isFourCCFormat(D3DFormat f) {
    switch (f) {
    case D3DFormat::DXT1:
    case D3DFormat::DXT2:
    case D3DFormat::DXT3:
    case D3DFormat::DXT4:
    case D3DFormat::DXT5:
    case D3DFormat::ATI1:
    case D3DFormat::ATI2:
    case D3DFormat::UYVY:
    case D3DFormat::YUY2:
    case D3DFormat::R8G8_B8G8:
    case D3DFormat::G8R8_G8B8:
    case D3DFormat::A16B16G16R16:
    case D3DFormat::Q16W16V16U16:
    case D3DFormat::R16F:
    case D3DFormat::G16R16F:
    case D3DFormat::A16B16G16R16F:
    case D3DFormat::R32F:
    case D3DFormat::G32R32F:
    case D3DFormat::A32B32G32R32F:
    case D3DFormat::CxV8U8:
    case D3DFormat::W11V11U10:
    case D3DFormat::A2B10G10R10_XR_BIAS:
        return true;
    default:
        return false;
    }
}

bool isBlockCompressed(D3DFormat f) {
    const hash::TextureFormatInfo info = hash::textureFormatInfo(f);
    return info.blockWidth == 4 && info.blockHeight == 4;
}

/// The canonical layout of one level (depth 1), valid only for single-plane formats with a known size.
std::optional<hash::TextureMip0Layout> levelLayout(D3DFormat f, std::uint32_t w, std::uint32_t h) {
    const hash::TextureMip0Layout l = hash::textureMip0Layout(f, w, h, 1);
    if (l.size == 0 || l.planes != 1) {
        return std::nullopt;
    }
    return l;
}

std::uint64_t ddsRowBytes(D3DFormat f, std::uint32_t w) {
    const hash::TextureFormatInfo info = hash::textureFormatInfo(f);
    const std::uint32_t blocksWide = (w + info.blockWidth - 1) / info.blockWidth;
    return std::uint64_t(info.elementSize) * blocksWide;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t levelExtent(std::uint32_t base, std::size_t level) { return std::max<std::uint32_t>(1u, base >> level); }

} // namespace

std::optional<DdsPixelFormat> ddsPixelFormat(D3DFormat format) {
    for (const MaskEntry& e : kMaskFormats) {
        if (e.format == format) {
            return e.pf;
        }
    }
    if (isFourCCFormat(format)) {
        DdsPixelFormat pf;
        pf.flags = ddpf::FourCC;
        pf.fourCC = static_cast<std::uint32_t>(format);
        return pf;
    }
    return std::nullopt;
}

D3DFormat d3dFormatFromDds(const DdsPixelFormat& pf) {
    if (pf.flags & ddpf::FourCC) {
        const D3DFormat f = static_cast<D3DFormat>(pf.fourCC);
        return isFourCCFormat(f) ? f : D3DFormat::Unknown;
    }
    for (const MaskEntry& e : kMaskFormats) {
        const DdsPixelFormat& m = e.pf;
        if (m.flags == pf.flags && m.rgbBitCount == pf.rgbBitCount && m.rMask == pf.rMask && m.gMask == pf.gMask &&
            m.bMask == pf.bMask && m.aMask == pf.aMask) {
            return e.format;
        }
    }
    return D3DFormat::Unknown;
}

std::uint64_t ddsLevelSize(D3DFormat format, std::uint32_t width, std::uint32_t height) {
    const auto l = levelLayout(format, width, height);
    return l ? ddsRowBytes(format, width) * l->blocksHigh : 0;
}

std::vector<std::uint8_t> writeDds(const DdsImage& image, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        return std::vector<std::uint8_t>();
    };
    const auto pf = ddsPixelFormat(image.format);
    if (!pf) {
        return fail("no DDS pixel format for D3DFORMAT " + std::to_string(static_cast<std::uint32_t>(image.format)));
    }
    if (image.width == 0 || image.height == 0 || image.mips.empty()) {
        return fail("empty image");
    }
    const auto l0 = levelLayout(image.format, image.width, image.height);
    if (!l0) {
        return fail("D3DFORMAT " + std::to_string(static_cast<std::uint32_t>(image.format)) + " has no single-plane layout");
    }
    const bool compressed = isBlockCompressed(image.format);
    std::vector<std::uint8_t> out;
    put32(out, kDdsMagic);
    put32(out, kHeaderSize);
    std::uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    flags |= compressed ? DDSD_LINEARSIZE : DDSD_PITCH;
    if (image.mips.size() > 1) {
        flags |= DDSD_MIPMAPCOUNT;
    }
    put32(out, flags);
    put32(out, image.height);
    put32(out, image.width);
    put32(out, static_cast<std::uint32_t>(compressed ? ddsLevelSize(image.format, image.width, image.height)
                                                     : ddsRowBytes(image.format, image.width)));
    put32(out, 0); // depth
    put32(out, static_cast<std::uint32_t>(image.mips.size()));
    for (int k = 0; k < 11; ++k) {
        put32(out, 0); // reserved1
    }
    put32(out, kPixelFormatSize);
    put32(out, pf->flags);
    put32(out, pf->fourCC);
    put32(out, pf->rgbBitCount);
    put32(out, pf->rMask);
    put32(out, pf->gMask);
    put32(out, pf->bMask);
    put32(out, pf->aMask);
    put32(out, DDSCAPS_TEXTURE | (image.mips.size() > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0));
    put32(out, 0); // caps2
    put32(out, 0); // caps3
    put32(out, 0); // caps4
    put32(out, 0); // reserved2
    for (std::size_t m = 0; m < image.mips.size(); ++m) {
        const std::uint32_t w = levelExtent(image.width, m), h = levelExtent(image.height, m);
        const auto l = levelLayout(image.format, w, h);
        const std::vector<std::uint8_t>& src = image.mips[m];
        if (!l || src.size() != l->size) {
            return fail("level " + std::to_string(m) + " has " + std::to_string(src.size()) + " bytes, the canonical layout " +
                        std::to_string(l ? l->size : 0));
        }
        const std::uint64_t rowBytes = ddsRowBytes(image.format, w);
        for (std::uint64_t r = 0; r < l->rowCount; ++r) {
            const std::uint8_t* row = src.data() + r * l->rowBytes;
            out.insert(out.end(), row, row + rowBytes);
        }
    }
    return out;
}

std::optional<DdsImage> readDds(std::span<const std::uint8_t> file, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<DdsImage> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (file.size() < 4 + kHeaderSize || get32(file.data()) != kDdsMagic) {
        return fail("not a DDS file");
    }
    const std::uint8_t* h = file.data() + 4;
    if (get32(h) != kHeaderSize || get32(h + 72) != kPixelFormatSize) {
        return fail("bad DDS header size");
    }
    const std::uint32_t flags = get32(h + 4);
    DdsImage img;
    img.height = get32(h + 8);
    img.width = get32(h + 12);
    const std::uint32_t mipCount = (flags & DDSD_MIPMAPCOUNT) ? std::max<std::uint32_t>(1u, get32(h + 24)) : 1u;
    DdsPixelFormat pf;
    pf.flags = get32(h + 76);
    pf.fourCC = get32(h + 80);
    pf.rgbBitCount = get32(h + 84);
    pf.rMask = get32(h + 88);
    pf.gMask = get32(h + 92);
    pf.bMask = get32(h + 96);
    pf.aMask = get32(h + 100);
    const std::uint32_t caps2 = get32(h + 108);
    if ((pf.flags & ddpf::FourCC) && pf.fourCC == fourCC('D', 'X', '1', '0')) {
        return fail("DX10 extension header is not supported");
    }
    if (caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)) {
        return fail("cube and volume DDS files are not supported");
    }
    img.format = d3dFormatFromDds(pf);
    if (img.format == D3DFormat::Unknown) {
        return fail("unknown DDS pixel format");
    }
    if (img.width == 0 || img.height == 0 || mipCount > 32) {
        return fail("bad extent or mip count");
    }
    std::size_t at = 4 + kHeaderSize;
    for (std::uint32_t m = 0; m < mipCount; ++m) {
        const std::uint32_t w = levelExtent(img.width, m), hh = levelExtent(img.height, m);
        const auto l = levelLayout(img.format, w, hh);
        if (!l) {
            return fail("format has no single-plane layout");
        }
        const std::uint64_t rowBytes = ddsRowBytes(img.format, w);
        const std::uint64_t size = rowBytes * l->rowCount;
        if (at + size > file.size()) {
            return fail("truncated level " + std::to_string(m));
        }
        std::vector<std::uint8_t> level(static_cast<std::size_t>(l->size), 0);
        for (std::uint64_t r = 0; r < l->rowCount; ++r) {
            std::memcpy(level.data() + r * l->rowBytes, file.data() + at + r * rowBytes, static_cast<std::size_t>(rowBytes));
        }
        at += static_cast<std::size_t>(size);
        img.mips.push_back(std::move(level));
    }
    if (at != file.size()) {
        return fail("trailing bytes after the last level");
    }
    return img;
}

// ---- RGBA8 decode ---------------------------------------------------------------------------------------

namespace {

std::uint8_t expand(std::uint32_t v, int bits) {
    if (bits == 0) {
        return 0;
    }
    const std::uint32_t maxv = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((v * 255u + maxv / 2u) / maxv);
}

std::uint32_t readTexel(const std::uint8_t* p, std::uint32_t bytes) {
    std::uint32_t v = 0;
    for (std::uint32_t k = 0; k < bytes; ++k) {
        v |= std::uint32_t(p[k]) << (8 * k);
    }
    return v;
}

/// Channel of a packed texel by mask (shift and width from the mask).
std::uint8_t channel(std::uint32_t texel, std::uint32_t mask) {
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    while (((mask >> shift) & 1u) == 0) {
        ++shift;
    }
    int bits = 0;
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1u)) {
        ++bits;
    }
    return expand((texel & mask) >> shift, bits);
}

void decodeBc1Block(const std::uint8_t* b, std::uint8_t out[16][4], bool forceFourColor) {
    const std::uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    std::uint8_t pal[4][4];
    auto rgb565 = [](std::uint32_t c, std::uint8_t* o) {
        o[0] = expand((c >> 11) & 31, 5);
        o[1] = expand((c >> 5) & 63, 6);
        o[2] = expand(c & 31, 5);
        o[3] = 255;
    };
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || forceFourColor) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const std::uint32_t bits = get32(b + 4);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
    }
}

void decodeBc3Alpha(const std::uint8_t* b, std::uint8_t out[16][4]) {
    const std::uint32_t a0 = b[0], a1 = b[1];
    std::uint8_t pal[8];
    pal[0] = static_cast<std::uint8_t>(a0);
    pal[1] = static_cast<std::uint8_t>(a1);
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((7 - k) * a0 + k * a1 + 3) / 7);
        }
    } else {
        for (int k = 1; k <= 4; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((5 - k) * a0 + k * a1 + 2) / 5);
        }
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) {
        bits |= std::uint64_t(b[2 + k]) << (8 * k);
    }
    for (int i = 0; i < 16; ++i) {
        out[i][3] = pal[(bits >> (3 * i)) & 7];
    }
}

} // namespace

std::optional<std::vector<std::uint8_t>> decodeRgba8(D3DFormat format, std::uint32_t width, std::uint32_t height,
                                                     std::span<const std::uint8_t> canonicalMip0) {
    const auto l = levelLayout(format, width, height);
    if (!l || canonicalMip0.size() != l->size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(std::size_t(width) * height * 4, 0);
    auto px = [&](std::uint32_t x, std::uint32_t y) { return out.data() + (std::size_t(y) * width + x) * 4; };

    if (format == D3DFormat::DXT1 || format == D3DFormat::DXT2 || format == D3DFormat::DXT3 || format == D3DFormat::DXT4 ||
        format == D3DFormat::DXT5) {
        const bool bc1 = format == D3DFormat::DXT1;
        const std::uint32_t blockBytes = bc1 ? 8 : 16;
        for (std::uint32_t by = 0; by < l->blocksHigh; ++by) {
            for (std::uint32_t bx = 0; bx < l->blocksWide; ++bx) {
                const std::uint8_t* b = canonicalMip0.data() + by * l->rowBytes + std::size_t(bx) * blockBytes;
                std::uint8_t texels[16][4];
                if (bc1) {
                    decodeBc1Block(b, texels, false);
                } else {
                    decodeBc1Block(b + 8, texels, true);
                    if (format == D3DFormat::DXT2 || format == D3DFormat::DXT3) {
                        for (int i = 0; i < 16; ++i) {
                            texels[i][3] = expand((b[i / 2] >> (4 * (i & 1))) & 15, 4);
                        }
                    } else {
                        decodeBc3Alpha(b, texels);
                    }
                }
                for (int i = 0; i < 16; ++i) {
                    const std::uint32_t x = bx * 4 + std::uint32_t(i % 4), y = by * 4 + std::uint32_t(i / 4);
                    if (x < width && y < height) {
                        std::memcpy(px(x, y), texels[i], 4);
                    }
                }
            }
        }
        return out;
    }

    // Uncompressed: RGB / luminance / alpha formats by their masks.
    const auto pf = ddsPixelFormat(format);
    if (!pf || (pf->flags & (ddpf::FourCC | ddpf::BumpDuDv | ddpf::BumpLuminance)) != 0) {
        if (format != D3DFormat::A16B16G16R16) {
            return std::nullopt;
        }
    }
    const std::uint32_t bytes = hash::textureFormatInfo(format).elementSize;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = canonicalMip0.data() + y * l->rowBytes;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* o = px(x, y);
            if (format == D3DFormat::A16B16G16R16) {
                const std::uint8_t* t = row + std::size_t(x) * 8;
                for (int c = 0; c < 4; ++c) {
                    const std::uint32_t v = t[2 * c] | (t[2 * c + 1] << 8);
                    o[c] = static_cast<std::uint8_t>((v * 255u + 32767u) / 65535u);
                }
                continue;
            }
            const std::uint32_t t = readTexel(row + std::size_t(x) * bytes, bytes);
            if (pf->flags & ddpf::Luminance) {
                o[0] = o[1] = o[2] = channel(t, pf->rMask);
                o[3] = (pf->flags & ddpf::AlphaPixels) ? channel(t, pf->aMask) : 255;
            } else if (pf->flags & ddpf::Alpha) {
                o[0] = o[1] = o[2] = 0;
                o[3] = channel(t, pf->aMask);
            } else {
                o[0] = channel(t, pf->rMask);
                o[1] = channel(t, pf->gMask);
                o[2] = channel(t, pf->bMask);
                o[3] = (pf->flags & ddpf::AlphaPixels) ? channel(t, pf->aMask) : 255;
            }
        }
    }
    return out;
}

} // namespace fuse::relight::capture::exporter
