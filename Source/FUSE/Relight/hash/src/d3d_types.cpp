// FUSE Relight: D3D9 enumeration helpers for the Remix-compatible hashes.
//
// declTypeVkFormat / topologyFromD3D / d3dVertexCount restate DXVK's DecodeDecltype,
// DecodeInputAssemblyState and GetVertexCount (src/d3d9/d3d9_util.cpp, zlib licence, Copyright (c)
// 2017-2021 Philip Rebohle, 2019-2021 Joshua Ashton; as used by dxvk-remix @0867d3c). Altered
// source: rewritten as plain tables over FUSE's own enumerations.
#include <fuse/relight/hash/d3d_types.hpp>

#include <array>

namespace fuse::relight::hash {

namespace {

struct FormatName {
    D3DFormat format;
    std::string_view name;
};

constexpr std::array kFormatNames = {
    FormatName{D3DFormat::Unknown, "Unknown"},
    FormatName{D3DFormat::R8G8B8, "R8G8B8"},
    FormatName{D3DFormat::A8R8G8B8, "A8R8G8B8"},
    FormatName{D3DFormat::X8R8G8B8, "X8R8G8B8"},
    FormatName{D3DFormat::R5G6B5, "R5G6B5"},
    FormatName{D3DFormat::X1R5G5B5, "X1R5G5B5"},
    FormatName{D3DFormat::A1R5G5B5, "A1R5G5B5"},
    FormatName{D3DFormat::A4R4G4B4, "A4R4G4B4"},
    FormatName{D3DFormat::R3G3B2, "R3G3B2"},
    FormatName{D3DFormat::A8, "A8"},
    FormatName{D3DFormat::A8R3G3B2, "A8R3G3B2"},
    FormatName{D3DFormat::X4R4G4B4, "X4R4G4B4"},
    FormatName{D3DFormat::A2B10G10R10, "A2B10G10R10"},
    FormatName{D3DFormat::A8B8G8R8, "A8B8G8R8"},
    FormatName{D3DFormat::X8B8G8R8, "X8B8G8R8"},
    FormatName{D3DFormat::G16R16, "G16R16"},
    FormatName{D3DFormat::A2R10G10B10, "A2R10G10B10"},
    FormatName{D3DFormat::A16B16G16R16, "A16B16G16R16"},
    FormatName{D3DFormat::A8P8, "A8P8"},
    FormatName{D3DFormat::P8, "P8"},
    FormatName{D3DFormat::L8, "L8"},
    FormatName{D3DFormat::A8L8, "A8L8"},
    FormatName{D3DFormat::A4L4, "A4L4"},
    FormatName{D3DFormat::V8U8, "V8U8"},
    FormatName{D3DFormat::L6V5U5, "L6V5U5"},
    FormatName{D3DFormat::X8L8V8U8, "X8L8V8U8"},
    FormatName{D3DFormat::Q8W8V8U8, "Q8W8V8U8"},
    FormatName{D3DFormat::V16U16, "V16U16"},
    FormatName{D3DFormat::W11V11U10, "W11V11U10"},
    FormatName{D3DFormat::A2W10V10U10, "A2W10V10U10"},
    FormatName{D3DFormat::UYVY, "UYVY"},
    FormatName{D3DFormat::R8G8_B8G8, "R8G8_B8G8"},
    FormatName{D3DFormat::YUY2, "YUY2"},
    FormatName{D3DFormat::G8R8_G8B8, "G8R8_G8B8"},
    FormatName{D3DFormat::DXT1, "DXT1"},
    FormatName{D3DFormat::DXT2, "DXT2"},
    FormatName{D3DFormat::DXT3, "DXT3"},
    FormatName{D3DFormat::DXT4, "DXT4"},
    FormatName{D3DFormat::DXT5, "DXT5"},
    FormatName{D3DFormat::D16_LOCKABLE, "D16_LOCKABLE"},
    FormatName{D3DFormat::D32, "D32"},
    FormatName{D3DFormat::D15S1, "D15S1"},
    FormatName{D3DFormat::D24S8, "D24S8"},
    FormatName{D3DFormat::D24X8, "D24X8"},
    FormatName{D3DFormat::D24X4S4, "D24X4S4"},
    FormatName{D3DFormat::D16, "D16"},
    FormatName{D3DFormat::D32F_LOCKABLE, "D32F_LOCKABLE"},
    FormatName{D3DFormat::D24FS8, "D24FS8"},
    FormatName{D3DFormat::D32_LOCKABLE, "D32_LOCKABLE"},
    FormatName{D3DFormat::S8_LOCKABLE, "S8_LOCKABLE"},
    FormatName{D3DFormat::L16, "L16"},
    FormatName{D3DFormat::VERTEXDATA, "VERTEXDATA"},
    FormatName{D3DFormat::INDEX16, "INDEX16"},
    FormatName{D3DFormat::INDEX32, "INDEX32"},
    FormatName{D3DFormat::Q16W16V16U16, "Q16W16V16U16"},
    FormatName{D3DFormat::MULTI2_ARGB8, "MULTI2_ARGB8"},
    FormatName{D3DFormat::R16F, "R16F"},
    FormatName{D3DFormat::G16R16F, "G16R16F"},
    FormatName{D3DFormat::A16B16G16R16F, "A16B16G16R16F"},
    FormatName{D3DFormat::R32F, "R32F"},
    FormatName{D3DFormat::G32R32F, "G32R32F"},
    FormatName{D3DFormat::A32B32G32R32F, "A32B32G32R32F"},
    FormatName{D3DFormat::CxV8U8, "CxV8U8"},
    FormatName{D3DFormat::A1, "A1"},
    FormatName{D3DFormat::A2B10G10R10_XR_BIAS, "A2B10G10R10_XR_BIAS"},
    FormatName{D3DFormat::BINARYBUFFER, "BINARYBUFFER"},
    FormatName{D3DFormat::ATI1, "ATI1"},
    FormatName{D3DFormat::ATI2, "ATI2"},
    FormatName{D3DFormat::INST, "INST"},
    FormatName{D3DFormat::DF24, "DF24"},
    FormatName{D3DFormat::DF16, "DF16"},
    FormatName{D3DFormat::NULL_FORMAT, "NULL_FORMAT"},
    FormatName{D3DFormat::GET4, "GET4"},
    FormatName{D3DFormat::GET1, "GET1"},
    FormatName{D3DFormat::NVDB, "NVDB"},
    FormatName{D3DFormat::A2M1, "A2M1"},
    FormatName{D3DFormat::A2M0, "A2M0"},
    FormatName{D3DFormat::ATOC, "ATOC"},
    FormatName{D3DFormat::INTZ, "INTZ"},
    FormatName{D3DFormat::RAWZ, "RAWZ"},
    FormatName{D3DFormat::RESZ, "RESZ"},
    FormatName{D3DFormat::NV11, "NV11"},
    FormatName{D3DFormat::NV12, "NV12"},
    FormatName{D3DFormat::P010, "P010"},
    FormatName{D3DFormat::P016, "P016"},
    FormatName{D3DFormat::Y210, "Y210"},
    FormatName{D3DFormat::Y216, "Y216"},
    FormatName{D3DFormat::Y410, "Y410"},
    FormatName{D3DFormat::AYUV, "AYUV"},
    FormatName{D3DFormat::YV12, "YV12"},
    FormatName{D3DFormat::OPAQUE_420, "OPAQUE_420"},
    FormatName{D3DFormat::AI44, "AI44"},
    FormatName{D3DFormat::IA44, "IA44"},
    FormatName{D3DFormat::R2VB, "R2VB"},
    FormatName{D3DFormat::COPM, "COPM"},
    FormatName{D3DFormat::SSAA, "SSAA"},
    FormatName{D3DFormat::AL16, "AL16"},
    FormatName{D3DFormat::R16, "R16"},
    FormatName{D3DFormat::EXT1, "EXT1"},
    FormatName{D3DFormat::FXT1, "FXT1"},
    FormatName{D3DFormat::GXT1, "GXT1"},
    FormatName{D3DFormat::HXT1, "HXT1"},
};

constexpr std::array<D3DFormat, kFormatNames.size()> makeFormatList() {
    std::array<D3DFormat, kFormatNames.size()> out{};
    for (std::size_t i = 0; i < kFormatNames.size(); ++i) {
        out[i] = kFormatNames[i].format;
    }
    return out;
}

constexpr auto kFormatList = makeFormatList();

} // namespace

std::span<const D3DFormat> allD3DFormats() noexcept {
    return kFormatList;
}

std::string_view d3dFormatName(D3DFormat format) noexcept {
    for (const FormatName& f : kFormatNames) {
        if (f.format == format) {
            return f.name;
        }
    }
    return {};
}

std::uint32_t declTypeVkFormat(D3DDeclType type) noexcept {
    namespace vf = vk_format;
    switch (type) {
    case D3DDeclType::Float1: return vf::kR32Sfloat;
    case D3DDeclType::Float2: return vf::kR32G32Sfloat;
    case D3DDeclType::Float3: return vf::kR32G32B32Sfloat;
    case D3DDeclType::Float4: return vf::kR32G32B32A32Sfloat;
    case D3DDeclType::D3DColor: return vf::kB8G8R8A8Unorm;
    case D3DDeclType::UByte4: return vf::kR8G8B8A8Uscaled;
    case D3DDeclType::Short2: return vf::kR16G16Sscaled;
    case D3DDeclType::Short4: return vf::kR16G16B16A16Sscaled;
    case D3DDeclType::UByte4N: return vf::kR8G8B8A8Unorm;
    case D3DDeclType::Short2N: return vf::kR16G16Snorm;
    case D3DDeclType::Short4N: return vf::kR16G16B16A16Snorm;
    case D3DDeclType::UShort2N: return vf::kR16G16Unorm;
    case D3DDeclType::UShort4N: return vf::kR16G16B16A16Unorm;
    case D3DDeclType::UDec3: return vf::kA2B10G10R10UscaledPack32;
    case D3DDeclType::Float16_2: return vf::kR16G16Sfloat;
    case D3DDeclType::Float16_4: return vf::kR16G16B16A16Sfloat;
    case D3DDeclType::Dec3N: return vf::kA2B10G10R10SnormPack32;
    case D3DDeclType::Unused: break;
    }
    return vf::kUndefined;
}

std::uint32_t declTypeElementSize(D3DDeclType type) noexcept {
    switch (type) {
    case D3DDeclType::Float1: return 4;
    case D3DDeclType::Float2: return 8;
    case D3DDeclType::Float3: return 12;
    case D3DDeclType::Float4: return 16;
    case D3DDeclType::D3DColor: return 4;
    case D3DDeclType::UByte4: return 4;
    case D3DDeclType::Short2: return 4;
    case D3DDeclType::Short4: return 8;
    case D3DDeclType::UByte4N: return 4;
    case D3DDeclType::Short2N: return 4;
    case D3DDeclType::Short4N: return 8;
    case D3DDeclType::UShort2N: return 4;
    case D3DDeclType::UShort4N: return 8;
    case D3DDeclType::UDec3: return 4;
    case D3DDeclType::Float16_2: return 4;
    case D3DDeclType::Float16_4: return 8;
    case D3DDeclType::Dec3N: return 4;
    case D3DDeclType::Unused: break;
    }
    return 0; // VK_FORMAT_UNDEFINED
}

Topology topologyFromD3D(D3DPrimitiveType type) noexcept {
    switch (type) {
    case D3DPrimitiveType::PointList: return Topology::PointList;
    case D3DPrimitiveType::LineList: return Topology::LineList;
    case D3DPrimitiveType::LineStrip: return Topology::LineStrip;
    case D3DPrimitiveType::TriangleStrip: return Topology::TriangleStrip;
    case D3DPrimitiveType::TriangleFan: return Topology::TriangleFan;
    case D3DPrimitiveType::TriangleList: break;
    }
    return Topology::TriangleList;
}

std::uint32_t d3dVertexCount(D3DPrimitiveType type, std::uint32_t count) noexcept {
    switch (type) {
    case D3DPrimitiveType::PointList: return count;
    case D3DPrimitiveType::LineList: return count * 2;
    case D3DPrimitiveType::LineStrip: return count + 1;
    case D3DPrimitiveType::TriangleStrip: return count + 2;
    case D3DPrimitiveType::TriangleFan: return count + 2;
    case D3DPrimitiveType::TriangleList: break;
    }
    return count * 3;
}

} // namespace fuse::relight::hash
