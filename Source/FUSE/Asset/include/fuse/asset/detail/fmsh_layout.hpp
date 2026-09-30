#pragma once

// UNI-U7-ASSET-1: FMSH (`.fusemesh`) layout constants shared by the runtime reader (fuse_asset) and
// the offline writer (Tools/FUSE/Cook). Not a public API: the format itself is documented on
// `fuse::cook::serialize_cooked_mesh`.

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/types.hpp>

namespace fuse::asset::detail {

inline constexpr u8 kFmshMagic[4] = {'F', 'M', 'S', 'H'};
inline constexpr usize kFmshHeaderBytes = 4u + 5u * 4u + 6u * 4u;
inline constexpr usize kFmshHeaderBytesV2 = kFmshHeaderBytes + 8u; // + streamCount + materialSlotCount
inline constexpr usize kFmshSubmeshBytes = 4u * 4u;
inline constexpr usize kFmshTrailerBytes = 8u;

inline constexpr u32 kFmshFlagQuantizedPositions = 1u;
inline constexpr u32 kFmshFlagMeshoptCodec = 1u << 4; // W0.2 (bits 1..3 reserved)
inline constexpr u32 kFmshFlagSections = 1u << 5;     // W0.2

constexpr u32 fmsh_fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

// Section ids (see fuse::cook::serialize_cooked_mesh). Meshlet / DAG ids and layouts are the renderer's
// FMLT chunks.
inline constexpr u32 kSectionLodt = fmsh_fourcc('L', 'O', 'D', 'T');
inline constexpr u32 kSectionLodr = fmsh_fourcc('L', 'O', 'D', 'R');
inline constexpr u32 kSectionLodi = fmsh_fourcc('L', 'O', 'D', 'I');
inline constexpr u32 kSectionLodz = fmsh_fourcc('L', 'O', 'D', 'Z');
inline constexpr u32 kSectionMlth = fmsh_fourcc('M', 'L', 'T', 'H');
inline constexpr u32 kSectionSubm = fmsh_fourcc('S', 'U', 'B', 'M');
inline constexpr u32 kSectionMshl = fmsh_fourcc('M', 'S', 'H', 'L');
inline constexpr u32 kSectionMvrt = fmsh_fourcc('M', 'V', 'R', 'T');
inline constexpr u32 kSectionMtri = fmsh_fourcc('M', 'T', 'R', 'I');
inline constexpr u32 kSectionDagh = fmsh_fourcc('D', 'A', 'G', 'H');
inline constexpr u32 kSectionDmsh = fmsh_fourcc('D', 'M', 'S', 'H');
inline constexpr u32 kSectionDmvr = fmsh_fourcc('D', 'M', 'V', 'R');
inline constexpr u32 kSectionDmtr = fmsh_fourcc('D', 'M', 'T', 'R');
inline constexpr u32 kSectionDgrp = fmsh_fourcc('D', 'G', 'R', 'P');
inline constexpr u32 kSectionDgmb = fmsh_fourcc('D', 'G', 'M', 'B');
inline constexpr u32 kSectionDclk = fmsh_fourcc('D', 'C', 'L', 'K');

inline constexpr u32 kDagTerminalBits = 0x7F7FFFFFu; // FLT_MAX

inline u64 fmsh_fnv1a64(const u8* data, usize size) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

inline u32 stream_element_bytes(MeshStreamFormat format) {
    switch (format) {
    case MeshStreamFormat::F32x2:
        return 8u;
    case MeshStreamFormat::F32x3:
        return 12u;
    case MeshStreamFormat::Unorm16x3:
        return 6u;
    case MeshStreamFormat::OctSnorm16x2:
        return 4u;
    case MeshStreamFormat::OctSnorm16x2Sign:
        return 8u;
    case MeshStreamFormat::Unorm8x4:
    case MeshStreamFormat::Uint8x4:
        return 4u;
    case MeshStreamFormat::Uint16x4:
    case MeshStreamFormat::Unorm16x4:
        return 8u;
    }
    return 0u;
}

inline bool stream_format_allowed(MeshStreamSemantic semantic, MeshStreamFormat format) {
    switch (semantic) {
    case MeshStreamSemantic::Position:
        return format == MeshStreamFormat::F32x3 || format == MeshStreamFormat::Unorm16x3;
    case MeshStreamSemantic::Normal:
        return format == MeshStreamFormat::F32x3 || format == MeshStreamFormat::OctSnorm16x2;
    case MeshStreamSemantic::Uv0:
    case MeshStreamSemantic::Uv1:
        return format == MeshStreamFormat::F32x2;
    case MeshStreamSemantic::Tangent:
        return format == MeshStreamFormat::OctSnorm16x2Sign;
    case MeshStreamSemantic::Color0:
        return format == MeshStreamFormat::Unorm8x4;
    case MeshStreamSemantic::Joints0:
        return format == MeshStreamFormat::Uint8x4 || format == MeshStreamFormat::Uint16x4;
    case MeshStreamSemantic::Weights0:
        return format == MeshStreamFormat::Unorm8x4 || format == MeshStreamFormat::Unorm16x4;
    }
    return false;
}

} // namespace fuse::asset::detail
