// FUSE Relight: the D3D9 and Vulkan enumeration values the Remix-compatible hashes depend on.
//
// The hash library is pure logic (no d3d9.h, no vulkan.h): the values below are restated from the
// public Direct3D 9 and Vulkan specifications. D3DFormat lists every value of DXVK's D3D9Format
// enumeration (src/d3d9/d3d9_format.h), including the FourCC driver-hack formats.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace fuse::relight::hash {

[[nodiscard]] constexpr std::uint32_t makeFourCC(char a, char b, char c, char d) noexcept {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) |
           (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24);
}

/// D3DFORMAT values.
enum class D3DFormat : std::uint32_t {
    Unknown = 0,
    R8G8B8 = 20,
    A8R8G8B8 = 21,
    X8R8G8B8 = 22,
    R5G6B5 = 23,
    X1R5G5B5 = 24,
    A1R5G5B5 = 25,
    A4R4G4B4 = 26,
    R3G3B2 = 27,
    A8 = 28,
    A8R3G3B2 = 29,
    X4R4G4B4 = 30,
    A2B10G10R10 = 31,
    A8B8G8R8 = 32,
    X8B8G8R8 = 33,
    G16R16 = 34,
    A2R10G10B10 = 35,
    A16B16G16R16 = 36,
    A8P8 = 40,
    P8 = 41,
    L8 = 50,
    A8L8 = 51,
    A4L4 = 52,
    V8U8 = 60,
    L6V5U5 = 61,
    X8L8V8U8 = 62,
    Q8W8V8U8 = 63,
    V16U16 = 64,
    W11V11U10 = 65,
    A2W10V10U10 = 67,
    UYVY = makeFourCC('U', 'Y', 'V', 'Y'),
    R8G8_B8G8 = makeFourCC('R', 'G', 'B', 'G'),
    YUY2 = makeFourCC('Y', 'U', 'Y', '2'),
    G8R8_G8B8 = makeFourCC('G', 'R', 'G', 'B'),
    DXT1 = makeFourCC('D', 'X', 'T', '1'),
    DXT2 = makeFourCC('D', 'X', 'T', '2'),
    DXT3 = makeFourCC('D', 'X', 'T', '3'),
    DXT4 = makeFourCC('D', 'X', 'T', '4'),
    DXT5 = makeFourCC('D', 'X', 'T', '5'),
    D16_LOCKABLE = 70,
    D32 = 71,
    D15S1 = 73,
    D24S8 = 75,
    D24X8 = 77,
    D24X4S4 = 79,
    D16 = 80,
    D32F_LOCKABLE = 82,
    D24FS8 = 83,
    D32_LOCKABLE = 84,
    S8_LOCKABLE = 85,
    L16 = 81,
    VERTEXDATA = 100,
    INDEX16 = 101,
    INDEX32 = 102,
    Q16W16V16U16 = 110,
    MULTI2_ARGB8 = makeFourCC('M', 'E', 'T', '1'),
    R16F = 111,
    G16R16F = 112,
    A16B16G16R16F = 113,
    R32F = 114,
    G32R32F = 115,
    A32B32G32R32F = 116,
    CxV8U8 = 117,
    A1 = 118,
    A2B10G10R10_XR_BIAS = 119,
    BINARYBUFFER = 199,
    // Driver hacks / unofficial formats.
    ATI1 = makeFourCC('A', 'T', 'I', '1'),
    ATI2 = makeFourCC('A', 'T', 'I', '2'),
    INST = makeFourCC('I', 'N', 'S', 'T'),
    DF24 = makeFourCC('D', 'F', '2', '4'),
    DF16 = makeFourCC('D', 'F', '1', '6'),
    NULL_FORMAT = makeFourCC('N', 'U', 'L', 'L'),
    GET4 = makeFourCC('G', 'E', 'T', '4'),
    GET1 = makeFourCC('G', 'E', 'T', '1'),
    NVDB = makeFourCC('N', 'V', 'D', 'B'),
    A2M1 = makeFourCC('A', '2', 'M', '1'),
    A2M0 = makeFourCC('A', '2', 'M', '0'),
    ATOC = makeFourCC('A', 'T', 'O', 'C'),
    INTZ = makeFourCC('I', 'N', 'T', 'Z'),
    RAWZ = makeFourCC('R', 'A', 'W', 'Z'),
    RESZ = makeFourCC('R', 'E', 'S', 'Z'),
    NV11 = makeFourCC('N', 'V', '1', '1'),
    NV12 = makeFourCC('N', 'V', '1', '2'),
    P010 = makeFourCC('P', '0', '1', '0'),
    P016 = makeFourCC('P', '0', '1', '6'),
    Y210 = makeFourCC('Y', '2', '1', '0'),
    Y216 = makeFourCC('Y', '2', '1', '6'),
    Y410 = makeFourCC('Y', '4', '1', '0'),
    AYUV = makeFourCC('A', 'Y', 'U', 'V'),
    YV12 = makeFourCC('Y', 'V', '1', '2'),
    OPAQUE_420 = makeFourCC('4', '2', '0', 'O'),
    AI44 = makeFourCC('A', 'I', '4', '4'),
    IA44 = makeFourCC('I', 'A', '4', '4'),
    R2VB = makeFourCC('R', '2', 'V', 'B'),
    COPM = makeFourCC('C', 'O', 'P', 'M'),
    SSAA = makeFourCC('S', 'S', 'A', 'A'),
    AL16 = makeFourCC('A', 'L', '1', '6'),
    R16 = makeFourCC(' ', 'R', '1', '6'),
    EXT1 = makeFourCC('E', 'X', 'T', '1'),
    FXT1 = makeFourCC('F', 'X', 'T', '1'),
    GXT1 = makeFourCC('G', 'X', 'T', '1'),
    HXT1 = makeFourCC('H', 'X', 'T', '1'),
};

/// Every D3DFormat enumerator above, in declaration order.
[[nodiscard]] std::span<const D3DFormat> allD3DFormats() noexcept;

/// The enumerator name ("A8R8G8B8", "DXT1", ...), or "" for a value that is not listed.
[[nodiscard]] std::string_view d3dFormatName(D3DFormat format) noexcept;

/// D3DDECLTYPE values.
enum class D3DDeclType : std::uint32_t {
    Float1 = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    D3DColor = 4,
    UByte4 = 5,
    Short2 = 6,
    Short4 = 7,
    UByte4N = 8,
    Short2N = 9,
    Short4N = 10,
    UShort2N = 11,
    UShort4N = 12,
    UDec3 = 13,
    Dec3N = 14,
    Float16_2 = 15,
    Float16_4 = 16,
    Unused = 17,
};

/// D3DPRIMITIVETYPE values.
enum class D3DPrimitiveType : std::uint32_t {
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriangleList = 4,
    TriangleStrip = 5,
    TriangleFan = 6,
};

/// D3DRESOURCETYPE values (only the texture type matters for hashing).
enum class D3DResourceType : std::uint32_t {
    Surface = 1,
    Volume = 2,
    Texture = 3,
    VolumeTexture = 4,
    CubeTexture = 5,
    VertexBuffer = 6,
    IndexBuffer = 7,
};

inline constexpr std::uint32_t kD3DUsageRenderTarget = 0x00000001u;
inline constexpr std::uint32_t kD3DUsageDepthStencil = 0x00000002u;

/// VkPrimitiveTopology values (the geometry-descriptor hash hashes these, not D3D's).
enum class Topology : std::uint32_t {
    PointList = 0,
    LineList = 1,
    LineStrip = 2,
    TriangleList = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
};

/// VkIndexType values.
enum class IndexType : std::uint32_t {
    Uint16 = 0,
    Uint32 = 1,
    NoneKhr = 1000165000,
};

/// The VkFormat values DXVK maps D3D formats and declaration types to (Vulkan specification values).
namespace vk_format {
inline constexpr std::uint32_t kUndefined = 0;
inline constexpr std::uint32_t kR4G4UnormPack8 = 1;
inline constexpr std::uint32_t kB4G4R4A4UnormPack16 = 3;
inline constexpr std::uint32_t kR5G6B5UnormPack16 = 4;
inline constexpr std::uint32_t kB5G6R5UnormPack16 = 5;
inline constexpr std::uint32_t kA1R5G5B5UnormPack16 = 8;
inline constexpr std::uint32_t kR8Unorm = 9;
inline constexpr std::uint32_t kR8Uint = 13;
inline constexpr std::uint32_t kR8Srgb = 15;
inline constexpr std::uint32_t kR8G8Unorm = 16;
inline constexpr std::uint32_t kR8G8Snorm = 17;
inline constexpr std::uint32_t kR8G8B8A8Unorm = 37;
inline constexpr std::uint32_t kR8G8B8A8Snorm = 38;
inline constexpr std::uint32_t kR8G8B8A8Uscaled = 39;
inline constexpr std::uint32_t kR8G8B8A8Srgb = 43;
inline constexpr std::uint32_t kB8G8R8A8Unorm = 44;
inline constexpr std::uint32_t kB8G8R8A8Srgb = 50;
inline constexpr std::uint32_t kA2R10G10B10UnormPack32 = 58;
inline constexpr std::uint32_t kA2B10G10R10UnormPack32 = 64;
inline constexpr std::uint32_t kA2B10G10R10SnormPack32 = 65;
inline constexpr std::uint32_t kA2B10G10R10UscaledPack32 = 66;
inline constexpr std::uint32_t kR16Unorm = 70;
inline constexpr std::uint32_t kR16Uint = 74;
inline constexpr std::uint32_t kR16Sfloat = 76;
inline constexpr std::uint32_t kR16G16Unorm = 77;
inline constexpr std::uint32_t kR16G16Snorm = 78;
inline constexpr std::uint32_t kR16G16Sscaled = 80;
inline constexpr std::uint32_t kR16G16Sfloat = 83;
inline constexpr std::uint32_t kR16G16B16A16Unorm = 91;
inline constexpr std::uint32_t kR16G16B16A16Snorm = 92;
inline constexpr std::uint32_t kR16G16B16A16Sscaled = 94;
inline constexpr std::uint32_t kR16G16B16A16Sfloat = 97;
inline constexpr std::uint32_t kR32Uint = 98;
inline constexpr std::uint32_t kR32Sfloat = 100;
inline constexpr std::uint32_t kR32G32Sfloat = 103;
inline constexpr std::uint32_t kR32G32B32Sfloat = 106;
inline constexpr std::uint32_t kR32G32B32A32Sfloat = 109;
inline constexpr std::uint32_t kB10G11R11UfloatPack32 = 122;
inline constexpr std::uint32_t kD16Unorm = 124;
inline constexpr std::uint32_t kD32Sfloat = 126;
inline constexpr std::uint32_t kS8Uint = 127;
inline constexpr std::uint32_t kD24UnormS8Uint = 129;
inline constexpr std::uint32_t kD32SfloatS8Uint = 130;
inline constexpr std::uint32_t kBc1RgbaUnormBlock = 133;
inline constexpr std::uint32_t kBc2UnormBlock = 135;
inline constexpr std::uint32_t kBc3UnormBlock = 137;
inline constexpr std::uint32_t kBc4UnormBlock = 139;
inline constexpr std::uint32_t kBc5UnormBlock = 141;
inline constexpr std::uint32_t kG8B8G8R8422Unorm = 1000156000;
inline constexpr std::uint32_t kB8G8R8G8422Unorm = 1000156001;
inline constexpr std::uint32_t kA4R4G4B4UnormPack16 = 1000340000;
} // namespace vk_format

/// DecodeDecltype: the VkFormat DXVK gives a vertex declaration type (kUndefined for Unused).
[[nodiscard]] std::uint32_t declTypeVkFormat(D3DDeclType type) noexcept;

/// imageFormatInfo(DecodeDecltype(type))->elementSize: the bytes one element occupies, which is
/// what the positions/texcoords hashes read per vertex (FLOAT3 = 12, FLOAT4 = 16, D3DCOLOR = 4...).
[[nodiscard]] std::uint32_t declTypeElementSize(D3DDeclType type) noexcept;

/// DecodeInputAssemblyState: D3DPRIMITIVETYPE -> VkPrimitiveTopology (unknown -> triangle list).
[[nodiscard]] Topology topologyFromD3D(D3DPrimitiveType type) noexcept;

/// GetVertexCount(type, primitiveCount): the index count of an indexed draw, or the vertex count of
/// a non-indexed one (unknown types count as triangle lists).
[[nodiscard]] std::uint32_t d3dVertexCount(D3DPrimitiveType type, std::uint32_t primitiveCount) noexcept;

} // namespace fuse::relight::hash
