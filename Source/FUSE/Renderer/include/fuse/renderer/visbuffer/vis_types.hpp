#pragma once

// WP-1.4 visibility buffer: records shared by the C++ side and the shaders
// (shaders/visbuffer/*.{glsl,comp,vert,frag,slang}). Keep them in sync; the static_asserts pin the
// layouts. Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::visbuffer {

/// Push constants of the raster pipelines (vertex + fragment, both targets): 96 bytes.
struct VisRasterPush {
    f32 viewProj[16] = {}; ///< column-major, Vulkan clip space, forward depth (cull_types.hpp conventions)
    u32 scene = 0;         ///< GpuScene::headerHandle()
    u32 target64 = 0;      ///< atomic target: bindless storage-image (R64_UINT) or storage-buffer handle
    u32 width = 0;         ///< target extent (buffer target: row pitch in words)
    u32 height = 0;
    u32 pad[4] = {0u, 0u, 0u, 0u};
};
static_assert(sizeof(VisRasterPush) == 96u && offsetof(VisRasterPush, scene) == 64u, "VisRasterPush layout");

/// vis64 kernel modes.
enum Vis64Mode : u32 {
    kVis64ModeClear = 0u,  ///< every word <- kVis64Clear
    kVis64ModeExport = 1u, ///< R32G32 visibility + R32F depth from the words
};

/// Push constants of vis64 (clear / export): 32 bytes.
struct Vis64Push {
    u32 target64 = 0; ///< bindless handle of the 64-bit target (storage image or storage buffer)
    u32 vis = 0;      ///< bindless storage-image handle of the R32G32_UINT visibility image
    u32 depth = 0;    ///< bindless storage-image handle of the R32F exported depth
    u32 width = 0;
    u32 height = 0;
    u32 mode = kVis64ModeClear;
    u32 pad[2] = {0u, 0u};
};
static_assert(sizeof(Vis64Push) == 32u, "Vis64Push layout");

/// VisDecodeTexel::flags (vis_common.glsl FUSE_VIS_DECODE_*).
enum VisDecodeFlag : u32 {
    kVisDecodeEmpty = 0u,      ///< the pixel has no geometry
    kVisDecodeOk = 1u,
    kVisDecodeBadId = 2u,      ///< instance / mesh / triangle out of range (never for a correct target)
    kVisDecodeDegenerate = 3u, ///< the triangle has zero area as seen from the pixel
};

/// Output of the full-frame decode (one per pixel, row-major): 16 bytes.
struct VisDecodeTexel {
    f32 depth = 1.f; ///< z / w of the triangle's plane at the pixel centre (the rasteriser's depth)
    f32 b1 = 0.f;    ///< perspective-correct barycentrics of vertices 1 and 2 (b0 = 1 - b1 - b2)
    f32 b2 = 0.f;
    u32 flags = kVisDecodeEmpty;
};
static_assert(sizeof(VisDecodeTexel) == 16u, "VisDecodeTexel layout");

/// Push constants of vis_decode: 96 bytes.
struct VisDecodePush {
    f32 viewProj[16] = {};
    u64 out = 0;    ///< device address of VisDecodeTexel[width * height]
    u32 scene = 0;  ///< GpuScene::headerHandle()
    u32 vis = 0;    ///< bindless storage-image handle of the R32G32_UINT visibility image
    u32 width = 0;
    u32 height = 0;
    u32 pad[2] = {0u, 0u};
};
static_assert(sizeof(VisDecodePush) == 96u && offsetof(VisDecodePush, out) == 64u, "VisDecodePush layout");

} // namespace fuse::renderer::visbuffer
