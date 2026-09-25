// FUSE Relight RL-3.3: texture formats and the in-memory texture image shared by the DDS reader and the
// `.pkg` package reader (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// Formats are named by their VkFormat value (TexFormat below) because that is what reaches the GPU and
// what Remix packages store (AssetDesc::format is a VkFormat narrowed to 8 bits). No Vulkan header is
// needed: the enum restates the values of the subset the readers can produce.
//
// Colour space (§4.6): the sRGB variant of a format follows the USD `inputs:*:colorSpace` hint, or the
// Remix parameter's own colour space when the hint is "auto" (`diffuse_texture` and
// `emissive_mask_texture` are sRGB, data maps are linear). The DDS file's own *_SRGB flag is only the
// default when neither is known (resolveColourSpace).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fuse::relight::mods::assets {

/// VkFormat values of every format the Relight texture readers produce.
enum class TexFormat : std::uint32_t {
    Undefined = 0,
    R5G6B5_UNORM_PACK16 = 4,
    A1R5G5B5_UNORM_PACK16 = 8,
    R8_UNORM = 9,
    R8_SNORM = 10,
    R8_UINT = 13,
    R8_SINT = 14,
    R8G8_UNORM = 16,
    R8G8_SNORM = 17,
    R8G8_UINT = 20,
    R8G8_SINT = 21,
    R8G8B8A8_UNORM = 37,
    R8G8B8A8_SNORM = 38,
    R8G8B8A8_UINT = 41,
    R8G8B8A8_SINT = 42,
    R8G8B8A8_SRGB = 43,
    B8G8R8A8_UNORM = 44,
    B8G8R8A8_SRGB = 50,
    A2R10G10B10_UNORM_PACK32 = 58,
    A2B10G10R10_UNORM_PACK32 = 64,
    A2B10G10R10_UINT_PACK32 = 68,
    R16_UNORM = 70,
    R16_SNORM = 71,
    R16_UINT = 74,
    R16_SINT = 75,
    R16_SFLOAT = 76,
    R16G16_UNORM = 77,
    R16G16_SNORM = 78,
    R16G16_UINT = 81,
    R16G16_SINT = 82,
    R16G16_SFLOAT = 83,
    R16G16B16A16_UNORM = 91,
    R16G16B16A16_SNORM = 92,
    R16G16B16A16_UINT = 95,
    R16G16B16A16_SINT = 96,
    R16G16B16A16_SFLOAT = 97,
    R32_UINT = 98,
    R32_SINT = 99,
    R32_SFLOAT = 100,
    R32G32_UINT = 101,
    R32G32_SINT = 102,
    R32G32_SFLOAT = 103,
    R32G32B32_UINT = 104,
    R32G32B32_SINT = 105,
    R32G32B32_SFLOAT = 106,
    R32G32B32A32_UINT = 107,
    R32G32B32A32_SINT = 108,
    R32G32B32A32_SFLOAT = 109,
    B10G11R11_UFLOAT_PACK32 = 122,
    E5B9G9R9_UFLOAT_PACK32 = 123,
    BC1_RGB_UNORM_BLOCK = 131,
    BC1_RGB_SRGB_BLOCK = 132,
    BC1_RGBA_UNORM_BLOCK = 133,
    BC1_RGBA_SRGB_BLOCK = 134,
    BC2_UNORM_BLOCK = 135,
    BC2_SRGB_BLOCK = 136,
    BC3_UNORM_BLOCK = 137,
    BC3_SRGB_BLOCK = 138,
    BC4_UNORM_BLOCK = 139,
    BC4_SNORM_BLOCK = 140,
    BC5_UNORM_BLOCK = 141,
    BC5_SNORM_BLOCK = 142,
    BC6H_UFLOAT_BLOCK = 143,
    BC6H_SFLOAT_BLOCK = 144,
    BC7_UNORM_BLOCK = 145,
    BC7_SRGB_BLOCK = 146,
};

/// How the components of a texel are stored (the decoder's element type).
enum class TexelKind : std::uint8_t { Unorm, Snorm, Uint, Sint, Float, Packed, Block };

struct TexFormatInfo {
    const char* name = "";
    std::uint8_t blockWidth = 1;   ///< 4 for BCn, else 1
    std::uint8_t blockHeight = 1;
    std::uint8_t bytesPerBlock = 0; ///< bytes per texel (uncompressed) or per 4x4 block (BCn)
    std::uint8_t channels = 0;
    TexelKind kind = TexelKind::Unorm;
    bool srgb = false;
};

/// Format facts; nullptr when `format` is not one of the TexFormat values above.
const TexFormatInfo* texFormatInfo(TexFormat format);
inline bool isBlockCompressed(TexFormat f) {
    const TexFormatInfo* i = texFormatInfo(f);
    return i && i->blockWidth == 4;
}
/// The sRGB / linear twin of `format` (itself when it has none).
TexFormat srgbVariant(TexFormat format);
TexFormat linearVariant(TexFormat format);
bool hasSrgbVariant(TexFormat format);

/// Tightly packed bytes of one row of blocks / of a whole level (depth slices stacked). 0 when the
/// format is unknown, an extent is 0 or the size overflows 64 bits.
std::uint64_t texRowPitch(TexFormat format, std::uint32_t width);
std::uint64_t texLevelSize(TexFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t depth);
inline std::uint32_t mipExtent(std::uint32_t base, std::uint32_t level) {
    return level >= 32 ? 1u : (base >> level ? base >> level : 1u);
}
/// floor(log2(max extent)) + 1.
std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height, std::uint32_t depth);

/// Component mapping applied when sampling (Vulkan VkComponentSwizzle semantics). Used for the DX9
/// formats that have no Vulkan twin but keep their bytes: L8 -> R8 (rrr1), A8L8 -> R8G8 (rrrg), A8 -> R8
/// (000r), X8R8G8B8 -> B8G8R8A8 (a = 1).
enum class Swz : std::uint8_t { R, G, B, A, Zero, One };
struct Swizzle {
    Swz r = Swz::R, g = Swz::G, b = Swz::B, a = Swz::A;
    bool identity() const { return r == Swz::R && g == Swz::G && b == Swz::B && a == Swz::A; }
    bool operator==(const Swizzle&) const = default;
};

enum class TexDimension : std::uint8_t { Tex1D, Tex2D, Tex3D, Cube };

/// One mip level of one array element / cube face, inside TextureImage::data.
struct Subresource {
    std::uint32_t layer = 0; ///< array element (a whole cube for Cube images)
    std::uint32_t face = 0;  ///< 0..5 for Cube, else 0
    std::uint32_t mip = 0;
    std::uint32_t width = 1, height = 1, depth = 1;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

/// A texture as the GPU upload wants it: tightly packed levels, BCn blocks exactly as stored in the file
/// (never re-encoded). Subresources are ordered layer-major, then face, then mip (the DDS file order).
struct TextureImage {
    TexFormat format = TexFormat::Undefined;
    Swizzle swizzle;
    TexDimension dimension = TexDimension::Tex2D;
    std::uint32_t width = 0, height = 0, depth = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t arraySize = 1; ///< array elements (cubes count once, see faces)
    std::uint32_t faces = 1;     ///< 6 for Cube
    bool premultipliedAlpha = false;
    std::vector<std::uint8_t> data;
    std::vector<Subresource> subresources;

    const Subresource* subresource(std::uint32_t layer, std::uint32_t face, std::uint32_t mip) const;
};

/// Lays out every subresource of `image` (dimension, extents, mips, arraySize, faces, format set)
/// tightly packed in file order, filling image.subresources and returning the total size (0 on overflow
/// or unknown format).
std::uint64_t layoutSubresources(TextureImage& image);

// ---- colour space (§4.6) ---------------------------------------------------------------------------------

enum class ColourSpaceHint : std::uint8_t { Auto, Srgb, Linear };

/// USD `inputs:<param>:colorSpace` token: "sRGB" -> Srgb, "raw" / "linear" / "lin_rec709" -> Linear,
/// anything else ("auto", empty) -> Auto.
ColourSpaceHint colourSpaceFromUsd(std::string_view token);
/// Remix material parameters sampled as sRGB colour: diffuse_texture, emissive_mask_texture (also with
/// the `inputs:` prefix). Every other texture parameter is linear data.
bool remixParameterIsSrgb(std::string_view parameter);
/// The format to create the image view with: the hint wins, "auto" follows the parameter's colour space
/// (an empty parameter keeps `format` as the file stored it).
TexFormat resolveColourSpace(TexFormat format, ColourSpaceHint hint, std::string_view parameter);

} // namespace fuse::relight::mods::assets
