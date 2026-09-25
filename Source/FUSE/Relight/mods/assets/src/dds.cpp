// FUSE Relight RL-3.3: DDS reader (DX9 + DX10 headers) and DX10 writer (see dds.hpp).
#include <fuse/relight/mods/assets/dds.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fuse::relight::mods::assets {

namespace {

using hash::D3DFormat;

constexpr std::uint32_t kDdsMagic = 0x20534444; // "DDS "
constexpr std::uint32_t kHeaderSize = 124;
constexpr std::uint32_t kPixelFormatSize = 32;
constexpr std::uint32_t kDx10HeaderSize = 20;

constexpr std::uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8, DDSD_PIXELFORMAT = 0x1000,
                        DDSD_MIPMAPCOUNT = 0x20000, DDSD_LINEARSIZE = 0x80000, DDSD_DEPTH = 0x800000;
constexpr std::uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
constexpr std::uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00, DDSCAPS2_VOLUME = 0x200000;
constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40,
                        DDPF_LUMINANCE = 0x20000, DDPF_BUMPDUDV = 0x80000;
constexpr std::uint32_t kDimTexture1D = 2, kDimTexture2D = 3, kDimTexture3D = 4;
constexpr std::uint32_t kMiscTextureCube = 0x4;
constexpr std::uint32_t kAlphaModeMask = 0x7, kAlphaModePremultiplied = 2;

constexpr std::uint32_t fourCC(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) | (std::uint32_t(std::uint8_t(c)) << 16) |
           (std::uint32_t(std::uint8_t(d)) << 24);
}

std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

constexpr Swizzle kIdentity{};
constexpr Swizzle kOpaque{Swz::R, Swz::G, Swz::B, Swz::One};
constexpr Swizzle kLuminance{Swz::R, Swz::R, Swz::R, Swz::One};
constexpr Swizzle kLuminanceAlpha{Swz::R, Swz::R, Swz::R, Swz::G};
constexpr Swizzle kAlphaOnly{Swz::Zero, Swz::Zero, Swz::Zero, Swz::R};

// ---- DXGI ------------------------------------------------------------------------------------------------

struct DxgiEntry {
    std::uint32_t dxgi;
    TexFormat format;
    Swizzle swizzle;
};

const DxgiEntry kDxgi[] = {
    {2, TexFormat::R32G32B32A32_SFLOAT, kIdentity},  {3, TexFormat::R32G32B32A32_UINT, kIdentity},
    {4, TexFormat::R32G32B32A32_SINT, kIdentity},    {6, TexFormat::R32G32B32_SFLOAT, kIdentity},
    {7, TexFormat::R32G32B32_UINT, kIdentity},       {8, TexFormat::R32G32B32_SINT, kIdentity},
    {10, TexFormat::R16G16B16A16_SFLOAT, kIdentity}, {11, TexFormat::R16G16B16A16_UNORM, kIdentity},
    {12, TexFormat::R16G16B16A16_UINT, kIdentity},   {13, TexFormat::R16G16B16A16_SNORM, kIdentity},
    {14, TexFormat::R16G16B16A16_SINT, kIdentity},   {16, TexFormat::R32G32_SFLOAT, kIdentity},
    {17, TexFormat::R32G32_UINT, kIdentity},         {18, TexFormat::R32G32_SINT, kIdentity},
    {24, TexFormat::A2B10G10R10_UNORM_PACK32, kIdentity}, {25, TexFormat::A2B10G10R10_UINT_PACK32, kIdentity},
    {26, TexFormat::B10G11R11_UFLOAT_PACK32, kIdentity},  {27, TexFormat::R8G8B8A8_UNORM, kIdentity},
    {28, TexFormat::R8G8B8A8_UNORM, kIdentity},      {29, TexFormat::R8G8B8A8_SRGB, kIdentity},
    {30, TexFormat::R8G8B8A8_UINT, kIdentity},       {31, TexFormat::R8G8B8A8_SNORM, kIdentity},
    {32, TexFormat::R8G8B8A8_SINT, kIdentity},       {34, TexFormat::R16G16_SFLOAT, kIdentity},
    {35, TexFormat::R16G16_UNORM, kIdentity},        {36, TexFormat::R16G16_UINT, kIdentity},
    {37, TexFormat::R16G16_SNORM, kIdentity},        {38, TexFormat::R16G16_SINT, kIdentity},
    {41, TexFormat::R32_SFLOAT, kIdentity},          {42, TexFormat::R32_UINT, kIdentity},
    {43, TexFormat::R32_SINT, kIdentity},            {49, TexFormat::R8G8_UNORM, kIdentity},
    {50, TexFormat::R8G8_UINT, kIdentity},           {51, TexFormat::R8G8_SNORM, kIdentity},
    {52, TexFormat::R8G8_SINT, kIdentity},           {54, TexFormat::R16_SFLOAT, kIdentity},
    {56, TexFormat::R16_UNORM, kIdentity},           {57, TexFormat::R16_UINT, kIdentity},
    {58, TexFormat::R16_SNORM, kIdentity},           {59, TexFormat::R16_SINT, kIdentity},
    {61, TexFormat::R8_UNORM, kIdentity},            {62, TexFormat::R8_UINT, kIdentity},
    {63, TexFormat::R8_SNORM, kIdentity},            {64, TexFormat::R8_SINT, kIdentity},
    {65, TexFormat::R8_UNORM, kAlphaOnly},           {67, TexFormat::E5B9G9R9_UFLOAT_PACK32, kIdentity},
    {70, TexFormat::BC1_RGBA_UNORM_BLOCK, kIdentity}, {71, TexFormat::BC1_RGBA_UNORM_BLOCK, kIdentity},
    {72, TexFormat::BC1_RGBA_SRGB_BLOCK, kIdentity}, {73, TexFormat::BC2_UNORM_BLOCK, kIdentity},
    {74, TexFormat::BC2_UNORM_BLOCK, kIdentity},     {75, TexFormat::BC2_SRGB_BLOCK, kIdentity},
    {76, TexFormat::BC3_UNORM_BLOCK, kIdentity},     {77, TexFormat::BC3_UNORM_BLOCK, kIdentity},
    {78, TexFormat::BC3_SRGB_BLOCK, kIdentity},      {79, TexFormat::BC4_UNORM_BLOCK, kIdentity},
    {80, TexFormat::BC4_UNORM_BLOCK, kIdentity},     {81, TexFormat::BC4_SNORM_BLOCK, kIdentity},
    {82, TexFormat::BC5_UNORM_BLOCK, kIdentity},     {83, TexFormat::BC5_UNORM_BLOCK, kIdentity},
    {84, TexFormat::BC5_SNORM_BLOCK, kIdentity},     {85, TexFormat::R5G6B5_UNORM_PACK16, kIdentity},
    {86, TexFormat::A1R5G5B5_UNORM_PACK16, kIdentity}, {87, TexFormat::B8G8R8A8_UNORM, kIdentity},
    {88, TexFormat::B8G8R8A8_UNORM, kOpaque},        {90, TexFormat::B8G8R8A8_UNORM, kIdentity},
    {91, TexFormat::B8G8R8A8_SRGB, kIdentity},       {92, TexFormat::B8G8R8A8_UNORM, kOpaque},
    {93, TexFormat::B8G8R8A8_SRGB, kOpaque},         {94, TexFormat::BC6H_UFLOAT_BLOCK, kIdentity},
    {95, TexFormat::BC6H_UFLOAT_BLOCK, kIdentity},   {96, TexFormat::BC6H_SFLOAT_BLOCK, kIdentity},
    {97, TexFormat::BC7_UNORM_BLOCK, kIdentity},     {98, TexFormat::BC7_UNORM_BLOCK, kIdentity},
    {99, TexFormat::BC7_SRGB_BLOCK, kIdentity},
};

// ---- DX9 legacy formats ------------------------------------------------------------------------------------

enum class Convert : std::uint8_t { None, R8G8B8, R3G3B2, A8R3G3B2, A4R4G4B4, X4R4G4B4, A4L4 };

struct LegacyEntry {
    D3DFormat d3d;
    TexFormat format;
    Swizzle swizzle;
    Convert convert;
    bool premultiplied;
};

const LegacyEntry kLegacy[] = {
    {D3DFormat::A8R8G8B8, TexFormat::B8G8R8A8_UNORM, kIdentity, Convert::None, false},
    {D3DFormat::X8R8G8B8, TexFormat::B8G8R8A8_UNORM, kOpaque, Convert::None, false},
    {D3DFormat::A8B8G8R8, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::None, false},
    {D3DFormat::X8B8G8R8, TexFormat::R8G8B8A8_UNORM, kOpaque, Convert::None, false},
    {D3DFormat::G16R16, TexFormat::R16G16_UNORM, kIdentity, Convert::None, false},
    {D3DFormat::A2B10G10R10, TexFormat::A2B10G10R10_UNORM_PACK32, kIdentity, Convert::None, false},
    {D3DFormat::A2R10G10B10, TexFormat::A2R10G10B10_UNORM_PACK32, kIdentity, Convert::None, false},
    {D3DFormat::R5G6B5, TexFormat::R5G6B5_UNORM_PACK16, kIdentity, Convert::None, false},
    {D3DFormat::A1R5G5B5, TexFormat::A1R5G5B5_UNORM_PACK16, kIdentity, Convert::None, false},
    {D3DFormat::X1R5G5B5, TexFormat::A1R5G5B5_UNORM_PACK16, kOpaque, Convert::None, false},
    {D3DFormat::L8, TexFormat::R8_UNORM, kLuminance, Convert::None, false},
    {D3DFormat::L16, TexFormat::R16_UNORM, kLuminance, Convert::None, false},
    {D3DFormat::A8L8, TexFormat::R8G8_UNORM, kLuminanceAlpha, Convert::None, false},
    {D3DFormat::A8, TexFormat::R8_UNORM, kAlphaOnly, Convert::None, false},
    {D3DFormat::V8U8, TexFormat::R8G8_SNORM, kIdentity, Convert::None, false},
    {D3DFormat::Q8W8V8U8, TexFormat::R8G8B8A8_SNORM, kIdentity, Convert::None, false},
    {D3DFormat::V16U16, TexFormat::R16G16_SNORM, kIdentity, Convert::None, false},
    {D3DFormat::A16B16G16R16, TexFormat::R16G16B16A16_UNORM, kIdentity, Convert::None, false},
    {D3DFormat::Q16W16V16U16, TexFormat::R16G16B16A16_SNORM, kIdentity, Convert::None, false},
    {D3DFormat::R16F, TexFormat::R16_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::G16R16F, TexFormat::R16G16_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::A16B16G16R16F, TexFormat::R16G16B16A16_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::R32F, TexFormat::R32_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::G32R32F, TexFormat::R32G32_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::A32B32G32R32F, TexFormat::R32G32B32A32_SFLOAT, kIdentity, Convert::None, false},
    {D3DFormat::DXT1, TexFormat::BC1_RGBA_UNORM_BLOCK, kIdentity, Convert::None, false},
    {D3DFormat::DXT2, TexFormat::BC2_UNORM_BLOCK, kIdentity, Convert::None, true},
    {D3DFormat::DXT3, TexFormat::BC2_UNORM_BLOCK, kIdentity, Convert::None, false},
    {D3DFormat::DXT4, TexFormat::BC3_UNORM_BLOCK, kIdentity, Convert::None, true},
    {D3DFormat::DXT5, TexFormat::BC3_UNORM_BLOCK, kIdentity, Convert::None, false},
    {D3DFormat::ATI1, TexFormat::BC4_UNORM_BLOCK, kIdentity, Convert::None, false},
    {D3DFormat::ATI2, TexFormat::BC5_UNORM_BLOCK, kIdentity, Convert::None, false},
    {D3DFormat::R8G8B8, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::R8G8B8, false},
    {D3DFormat::R3G3B2, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::R3G3B2, false},
    {D3DFormat::A8R3G3B2, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::A8R3G3B2, false},
    {D3DFormat::A4R4G4B4, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::A4R4G4B4, false},
    {D3DFormat::X4R4G4B4, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::X4R4G4B4, false},
    {D3DFormat::A4L4, TexFormat::R8G8B8A8_UNORM, kIdentity, Convert::A4L4, false},
};

const LegacyEntry* legacyEntry(D3DFormat f) {
    for (const LegacyEntry& e : kLegacy) {
        if (e.d3d == f) {
            return &e;
        }
    }
    return nullptr;
}

struct MaskEntry {
    std::uint32_t category; // DDPF_RGB / DDPF_LUMINANCE / DDPF_ALPHA / DDPF_BUMPDUDV
    std::uint32_t bits, r, g, b, a;
    D3DFormat format;
};

// D3DX's DX9 mask descriptions (the table capture/export's writer emits) plus the variants DirectXTex
// documents from other writers (R32F as a 32-bit red mask, L8 / A8L8 spelled as RGB).
const MaskEntry kMasks[] = {
    {DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0xff000000, D3DFormat::A8R8G8B8},
    {DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0, D3DFormat::X8R8G8B8},
    {DDPF_RGB, 32, 0xff, 0xff00, 0xff0000, 0xff000000, D3DFormat::A8B8G8R8},
    {DDPF_RGB, 32, 0xff, 0xff00, 0xff0000, 0, D3DFormat::X8B8G8R8},
    {DDPF_RGB, 32, 0xffff, 0xffff0000, 0, 0, D3DFormat::G16R16},
    {DDPF_RGB, 32, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000, D3DFormat::A2B10G10R10},
    {DDPF_RGB, 32, 0x3ff00000, 0xffc00, 0x3ff, 0xc0000000, D3DFormat::A2R10G10B10},
    {DDPF_RGB, 32, 0xffffffff, 0, 0, 0, D3DFormat::R32F},
    {DDPF_RGB, 24, 0xff0000, 0xff00, 0xff, 0, D3DFormat::R8G8B8},
    {DDPF_RGB, 16, 0xf800, 0x7e0, 0x1f, 0, D3DFormat::R5G6B5},
    {DDPF_RGB, 16, 0x7c00, 0x3e0, 0x1f, 0x8000, D3DFormat::A1R5G5B5},
    {DDPF_RGB, 16, 0x7c00, 0x3e0, 0x1f, 0, D3DFormat::X1R5G5B5},
    {DDPF_RGB, 16, 0xf00, 0xf0, 0xf, 0xf000, D3DFormat::A4R4G4B4},
    {DDPF_RGB, 16, 0xf00, 0xf0, 0xf, 0, D3DFormat::X4R4G4B4},
    {DDPF_RGB, 16, 0xe0, 0x1c, 0x3, 0xff00, D3DFormat::A8R3G3B2},
    {DDPF_RGB, 16, 0xff, 0, 0, 0xff00, D3DFormat::A8L8},
    {DDPF_RGB, 8, 0xe0, 0x1c, 0x3, 0, D3DFormat::R3G3B2},
    {DDPF_RGB, 8, 0xff, 0, 0, 0, D3DFormat::L8},
    {DDPF_LUMINANCE, 8, 0xff, 0, 0, 0, D3DFormat::L8},
    {DDPF_LUMINANCE, 16, 0xffff, 0, 0, 0, D3DFormat::L16},
    {DDPF_LUMINANCE, 16, 0xff, 0, 0, 0xff00, D3DFormat::A8L8},
    {DDPF_LUMINANCE, 8, 0xf, 0, 0, 0xf0, D3DFormat::A4L4},
    {DDPF_ALPHA, 8, 0, 0, 0, 0xff, D3DFormat::A8},
    {DDPF_BUMPDUDV, 16, 0xff, 0xff00, 0, 0, D3DFormat::V8U8},
    {DDPF_BUMPDUDV, 32, 0xff, 0xff00, 0xff0000, 0xff000000, D3DFormat::Q8W8V8U8},
    {DDPF_BUMPDUDV, 32, 0xffff, 0xffff0000, 0, 0, D3DFormat::V16U16},
};

struct PixelFormat {
    std::uint32_t flags, fourCC, bits, r, g, b, a;
};

D3DFormat legacyFromPixelFormat(const PixelFormat& pf) {
    if (pf.flags & DDPF_FOURCC) {
        switch (pf.fourCC) {
        case fourCC('B', 'C', '4', 'U'):
            return D3DFormat::ATI1;
        case fourCC('B', 'C', '5', 'U'):
            return D3DFormat::ATI2;
        default:
            break;
        }
        const D3DFormat f = static_cast<D3DFormat>(pf.fourCC);
        return legacyEntry(f) ? f : D3DFormat::Unknown;
    }
    for (const MaskEntry& m : kMasks) {
        if ((pf.flags & m.category) && pf.bits == m.bits && pf.r == m.r && pf.g == m.g && pf.b == m.b && pf.a == m.a) {
            return m.format;
        }
    }
    return D3DFormat::Unknown;
}

// ---- conversions to R8G8B8A8 --------------------------------------------------------------------------------

std::uint32_t convertSourceBytes(Convert c) {
    switch (c) {
    case Convert::R8G8B8:
        return 3;
    case Convert::R3G3B2:
    case Convert::A4L4:
        return 1;
    case Convert::A8R3G3B2:
    case Convert::A4R4G4B4:
    case Convert::X4R4G4B4:
        return 2;
    case Convert::None:
        break;
    }
    return 0;
}

std::uint8_t expandBits(std::uint32_t v, std::uint32_t bits) {
    const std::uint32_t maxv = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((v * 255u + maxv / 2u) / maxv);
}

void convertTexel(Convert c, const std::uint8_t* s, std::uint8_t* d) {
    switch (c) {
    case Convert::R8G8B8: // B, G, R in memory
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
        return;
    case Convert::R3G3B2:
        d[0] = expandBits(s[0] >> 5, 3);
        d[1] = expandBits((s[0] >> 2) & 7, 3);
        d[2] = expandBits(s[0] & 3, 2);
        d[3] = 255;
        return;
    case Convert::A8R3G3B2:
        d[0] = expandBits(s[0] >> 5, 3);
        d[1] = expandBits((s[0] >> 2) & 7, 3);
        d[2] = expandBits(s[0] & 3, 2);
        d[3] = s[1];
        return;
    case Convert::A4R4G4B4:
    case Convert::X4R4G4B4: {
        const std::uint32_t v = std::uint32_t(s[0]) | (std::uint32_t(s[1]) << 8);
        d[0] = expandBits((v >> 8) & 15, 4);
        d[1] = expandBits((v >> 4) & 15, 4);
        d[2] = expandBits(v & 15, 4);
        d[3] = c == Convert::A4R4G4B4 ? expandBits(v >> 12, 4) : 255;
        return;
    }
    case Convert::A4L4:
        d[0] = d[1] = d[2] = expandBits(s[0] & 15, 4);
        d[3] = expandBits(s[0] >> 4, 4);
        return;
    case Convert::None:
        break;
    }
}

template <typename Fn>
std::optional<DdsTexture> failWith(std::string* error, Fn&& why) {
    if (error) {
        *error = why();
    }
    return std::nullopt;
}

} // namespace

TexFormat texFormatFromDxgi(std::uint32_t dxgiFormat, Swizzle* swizzle) {
    for (const DxgiEntry& e : kDxgi) {
        if (e.dxgi == dxgiFormat) {
            if (swizzle) {
                *swizzle = e.swizzle;
            }
            return e.format;
        }
    }
    return TexFormat::Undefined;
}

std::uint32_t dxgiFromTexFormat(TexFormat format) {
    // Prefer the typed (non-TYPELESS) value: the table lists TYPELESS first for BCn, so pick the last
    // identity-swizzle match.
    std::uint32_t found = 0;
    for (const DxgiEntry& e : kDxgi) {
        if (e.format == format && e.swizzle.identity()) {
            found = e.dxgi;
            if (e.dxgi != 27 && e.dxgi != 90 && e.dxgi != 70 && e.dxgi != 73 && e.dxgi != 76 && e.dxgi != 79 && e.dxgi != 82 &&
                e.dxgi != 94 && e.dxgi != 97) {
                return found;
            }
        }
    }
    return found;
}

std::optional<DdsTexture> readDds(std::span<const std::uint8_t> file, std::string* error) {
    auto fail = [&](const char* why) { return failWith(error, [&] { return std::string(why); }); };
    if (file.size() < 4 + kHeaderSize || get32(file.data()) != kDdsMagic) {
        return fail("not a DDS file");
    }
    const std::uint8_t* h = file.data() + 4;
    if (get32(h) != kHeaderSize || get32(h + 72) != kPixelFormatSize) {
        return fail("bad DDS header size");
    }
    const std::uint32_t flags = get32(h + 4);
    DdsTexture tex;
    TextureImage& img = tex.image;
    img.height = get32(h + 8);
    img.width = get32(h + 12);
    const std::uint32_t headerDepth = get32(h + 20);
    const std::uint32_t mipField = get32(h + 24);
    PixelFormat pf{get32(h + 76), get32(h + 80), get32(h + 84), get32(h + 88), get32(h + 92), get32(h + 96), get32(h + 100)};
    const std::uint32_t caps2 = get32(h + 108);
    std::size_t dataStart = 4 + kHeaderSize;
    Convert convert = Convert::None;

    if ((pf.flags & DDPF_FOURCC) && pf.fourCC == fourCC('D', 'X', '1', '0')) {
        if (file.size() < dataStart + kDx10HeaderSize) {
            return fail("truncated DX10 header");
        }
        const std::uint8_t* x = file.data() + dataStart;
        dataStart += kDx10HeaderSize;
        tex.info.dx10Header = true;
        tex.info.dxgiFormat = get32(x);
        const std::uint32_t dim = get32(x + 4);
        const std::uint32_t misc = get32(x + 8);
        img.arraySize = get32(x + 12);
        img.premultipliedAlpha = (get32(x + 16) & kAlphaModeMask) == kAlphaModePremultiplied;
        img.format = texFormatFromDxgi(tex.info.dxgiFormat, &img.swizzle);
        if (img.format == TexFormat::Undefined) {
            return failWith(error, [&] { return "unsupported DXGI format " + std::to_string(tex.info.dxgiFormat); });
        }
        if (img.arraySize == 0) {
            return fail("DX10 array size is 0");
        }
        switch (dim) {
        case kDimTexture1D:
            if (img.height > 1) {
                return fail("1D texture with a height");
            }
            img.height = 1;
            img.dimension = TexDimension::Tex1D;
            break;
        case kDimTexture2D:
            img.dimension = (misc & kMiscTextureCube) ? TexDimension::Cube : TexDimension::Tex2D;
            break;
        case kDimTexture3D:
            if (img.arraySize != 1) {
                return fail("3D texture arrays are not valid");
            }
            img.dimension = TexDimension::Tex3D;
            img.depth = headerDepth;
            break;
        default:
            return fail("bad DX10 resource dimension");
        }
    } else {
        tex.info.legacyFormat = legacyFromPixelFormat(pf);
        const LegacyEntry* e = legacyEntry(tex.info.legacyFormat);
        if (!e) {
            return fail("unsupported DDS pixel format");
        }
        img.format = e->format;
        img.swizzle = e->swizzle;
        img.premultipliedAlpha = e->premultiplied;
        convert = e->convert;
        tex.info.converted = convert != Convert::None;
        if (caps2 & DDSCAPS2_CUBEMAP) {
            if ((caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES) {
                return fail("partial cube maps are not supported");
            }
            img.dimension = TexDimension::Cube;
        } else if ((caps2 & DDSCAPS2_VOLUME) && (flags & DDSD_DEPTH)) {
            img.dimension = TexDimension::Tex3D;
            img.depth = headerDepth;
        }
    }
    img.faces = img.dimension == TexDimension::Cube ? 6u : 1u;
    if (img.dimension == TexDimension::Cube && img.width != img.height) {
        return fail("cube map faces are not square");
    }
    if (img.width == 0 || img.height == 0 || img.depth == 0 || img.width > kDdsMaxExtent || img.height > kDdsMaxExtent ||
        img.depth > kDdsMaxDepth || img.arraySize > kDdsMaxArraySize) {
        return fail("bad extent or array size");
    }
    img.mipLevels = mipField == 0 ? 1u : mipField;
    if (img.mipLevels > fullMipCount(img.width, img.height, img.depth)) {
        return fail("more mip levels than the extent allows");
    }

    const std::uint64_t total = layoutSubresources(img);
    if (total == 0) {
        return fail("bad texture layout");
    }
    const std::uint64_t available = file.size() - dataStart;
    if (convert == Convert::None) {
        if (total > available) {
            return fail("truncated texture data");
        }
        img.data.assign(file.begin() + static_cast<std::ptrdiff_t>(dataStart),
                        file.begin() + static_cast<std::ptrdiff_t>(dataStart + total));
        tex.info.trailingBytes = available - total;
        return tex;
    }

    // Expand to R8G8B8A8: the file holds `srcBytes` per texel, tightly packed in the same order.
    const std::uint32_t srcBytes = convertSourceBytes(convert);
    std::uint64_t srcTotal = 0;
    for (const Subresource& s : img.subresources) {
        srcTotal += std::uint64_t(s.width) * s.height * s.depth * srcBytes;
    }
    if (srcTotal > available) {
        return fail("truncated texture data");
    }
    img.data.assign(static_cast<std::size_t>(total), 0);
    const std::uint8_t* src = file.data() + dataStart;
    for (const Subresource& s : img.subresources) {
        const std::uint64_t texels = std::uint64_t(s.width) * s.height * s.depth;
        std::uint8_t* dst = img.data.data() + s.offset;
        for (std::uint64_t t = 0; t < texels; ++t) {
            convertTexel(convert, src + t * srcBytes, dst + t * 4);
        }
        src += texels * srcBytes;
    }
    tex.info.trailingBytes = available - srcTotal;
    return tex;
}

std::optional<DdsTexture> readDdsFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return failWith(error, [&] { return "cannot open " + path.string(); });
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return readDds(bytes, error);
}

std::vector<std::uint8_t> writeDds(const TextureImage& image, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        return std::vector<std::uint8_t>();
    };
    const std::uint32_t dxgi = dxgiFromTexFormat(image.format);
    if (dxgi == 0 || !image.swizzle.identity()) {
        return fail("format has no DXGI spelling");
    }
    TextureImage layout = image;
    const std::uint64_t total = layoutSubresources(layout);
    if (total == 0 || total != image.data.size()) {
        return fail("data size does not match the layout");
    }
    const bool compressed = isBlockCompressed(image.format);
    const bool cube = image.dimension == TexDimension::Cube;
    const bool volume = image.dimension == TexDimension::Tex3D;
    std::vector<std::uint8_t> out;
    out.reserve(4 + kHeaderSize + kDx10HeaderSize + image.data.size());
    put32(out, kDdsMagic);
    put32(out, kHeaderSize);
    std::uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    flags |= compressed ? DDSD_LINEARSIZE : DDSD_PITCH;
    flags |= image.mipLevels > 1 ? DDSD_MIPMAPCOUNT : 0;
    flags |= volume ? DDSD_DEPTH : 0;
    put32(out, flags);
    put32(out, image.height);
    put32(out, image.width);
    put32(out, static_cast<std::uint32_t>(compressed ? texLevelSize(image.format, image.width, image.height, 1)
                                                     : texRowPitch(image.format, image.width)));
    put32(out, volume ? image.depth : 0);
    put32(out, image.mipLevels);
    for (int k = 0; k < 11; ++k) {
        put32(out, 0);
    }
    put32(out, kPixelFormatSize);
    put32(out, DDPF_FOURCC);
    put32(out, fourCC('D', 'X', '1', '0'));
    for (int k = 0; k < 5; ++k) {
        put32(out, 0);
    }
    const bool complex = image.mipLevels > 1 || cube || image.arraySize > 1 || volume;
    put32(out, DDSCAPS_TEXTURE | (complex ? DDSCAPS_COMPLEX : 0) | (image.mipLevels > 1 ? DDSCAPS_MIPMAP : 0));
    put32(out, (cube ? DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES : 0) | (volume ? DDSCAPS2_VOLUME : 0));
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, dxgi);
    put32(out, image.dimension == TexDimension::Tex1D ? kDimTexture1D : volume ? kDimTexture3D : kDimTexture2D);
    put32(out, cube ? kMiscTextureCube : 0);
    put32(out, image.arraySize);
    put32(out, image.premultipliedAlpha ? kAlphaModePremultiplied : 0);
    out.insert(out.end(), image.data.begin(), image.data.end());
    return out;
}

} // namespace fuse::relight::mods::assets
