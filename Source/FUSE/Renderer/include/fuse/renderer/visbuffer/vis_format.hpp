#pragma once

// WP-1.4 visibility buffer: the pixel formats. THE place the packing is defined (renderer plan
// Phase 1 "a 64-bit target storing depth plus instance and triangle ID, written with atomics or a
// raster pass"); shader twins shaders/visbuffer/vis_format.{glsl,slang}. The WP-5.x software
// rasteriser writes the same 64-bit words, and WP-1.5's resolve decodes both targets through these
// functions. fuse_rp_visbuffer_cpu pins every constant; fuse_rp_visbuffer compares GPU words with them.
//
// Raster target (T0 default): VK_FORMAT_R32G32_UINT colour attachment + D32 depth (LESS):
//   x = instance slot (GpuScene), y = triangle of the instance's mesh (MTRI index == gl_PrimitiveID
//   of the mesh's draw range, gpu_scene_types.hpp "Index layout"). Cleared to {~0u, ~0u}.
//
// Atomic target: one 64-bit word per pixel, written with atomicMin (R64_UINT storage image with
// VK_EXT_shader_image_atomic_int64, else a u64 storage buffer with shaderBufferInt64Atomics):
//   bits 63..40  depth, unorm24 = floor(depth * 2^24) clamped to 2^24 - 1 (forward depth: smaller wins)
//   bits 39..20  instance slot   (20 bits, 0xFFFFF = no geometry)
//   bits 19..0   mesh triangle   (20 bits)
//   cleared to ~0 (depth 2^24 - 1 and the invalid instance: farther than every sample).
// Why 24 + 20 + 20 and not 32-bit float depth + a 32-bit cluster id: until WP-5.x there is no
// per-frame visible-cluster list to index, and instance + triangle decode directly through the GPU
// scene. unorm24 is D24's precision; the floor makes the quantised depth monotone, so atomicMin keeps
// the nearest sample, and ties inside one quantum resolve to the smaller (instance, triangle), i.e.
// the result does not depend on draw order. Limits: instance slots < 0xFFFFF, mesh triangles
// <= 0xFFFFF (VisBuffer::beginFrame checks the instance limit).
// Depth for the Hi-Z (export pass): the far end of the quantum, min((q + 1) / 2^24, 1), so a
// pyramid built from it is conservative (never nearer than the surface).
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

namespace fuse::renderer::visbuffer {

inline constexpr u32 kVisInvalid = 0xFFFFFFFFu;
inline constexpr u32 kVisRasterFormat = 101u; ///< VK_FORMAT_R32G32_UINT
inline constexpr u32 kVisDepthFormat = 126u;  ///< VK_FORMAT_D32_SFLOAT (raster target)
inline constexpr u32 kVis64ImageFormat = 110u; ///< VK_FORMAT_R64_UINT (atomic image target)
inline constexpr u32 kVisExportDepthFormat = 100u; ///< VK_FORMAT_R32_SFLOAT (atomic target's exported depth)

inline constexpr u32 kVis64DepthBits = 24u;
inline constexpr u32 kVis64InstanceBits = 20u;
inline constexpr u32 kVis64TriangleBits = 20u;
static_assert(kVis64DepthBits + kVis64InstanceBits + kVis64TriangleBits == 64u, "64-bit visibility word");
inline constexpr u32 kVis64InstanceShift = kVis64TriangleBits;
inline constexpr u32 kVis64DepthShift = kVis64TriangleBits + kVis64InstanceBits;
inline constexpr u32 kVis64IdMask = 0xFFFFFu;
inline constexpr u32 kVis64DepthMax = (1u << kVis64DepthBits) - 1u;
/// Largest instance slot the atomic target can store (0xFFFFF marks "no geometry").
inline constexpr u32 kVis64MaxInstances = kVis64IdMask;
/// Triangles per mesh the atomic target can store.
inline constexpr u32 kVis64MaxTriangles = kVis64IdMask + 1u;
inline constexpr u64 kVis64Clear = ~u64{0};
inline constexpr f32 kVis64DepthScale = 16777216.f; ///< 2^24
inline constexpr f32 kVis64DepthUnit = 1.f / 16777216.f;

/// One decoded visibility sample (both targets).
struct VisSample {
    u32 instance = kVisInvalid;
    u32 triangle = kVisInvalid;
};

FUSE_HOST_DEVICE inline bool vis_valid(const VisSample& s) { return s.instance != kVisInvalid; }

/// floor(depth * 2^24) clamped to [0, 2^24 - 1]; not-greater-than-zero (and NaN) -> 0.
FUSE_HOST_DEVICE inline u32 vis64_quantize_depth(f32 depth) {
    if (!(depth > 0.f)) {
        return 0u;
    }
    if (depth >= 1.f) {
        return kVis64DepthMax;
    }
    const u32 q = static_cast<u32>(depth * kVis64DepthScale); // exact product, truncation == floor
    return q < kVis64DepthMax ? q : kVis64DepthMax;
}

/// Lower end of a depth quantum (q / 2^24, exact).
FUSE_HOST_DEVICE inline f32 vis64_depth_floor(u32 q) { return static_cast<f32>(q) * kVis64DepthUnit; }

FUSE_HOST_DEVICE inline u64 vis64_pack(f32 depth, u32 instance, u32 triangle) {
    return (static_cast<u64>(vis64_quantize_depth(depth)) << kVis64DepthShift) |
           (static_cast<u64>(instance & kVis64IdMask) << kVis64InstanceShift) | static_cast<u64>(triangle & kVis64IdMask);
}

FUSE_HOST_DEVICE inline u32 vis64_instance(u64 v) { return static_cast<u32>(v >> kVis64InstanceShift) & kVis64IdMask; }
FUSE_HOST_DEVICE inline u32 vis64_triangle(u64 v) { return static_cast<u32>(v) & kVis64IdMask; }
FUSE_HOST_DEVICE inline u32 vis64_depth_bits(u64 v) { return static_cast<u32>(v >> kVis64DepthShift); }
FUSE_HOST_DEVICE inline bool vis64_valid(u64 v) { return vis64_instance(v) != kVis64IdMask; }

/// Raster-format sample of a 64-bit word (invalid -> both kVisInvalid). The export pass writes it.
FUSE_HOST_DEVICE inline VisSample vis64_unpack(u64 v) {
    VisSample s{};
    if (vis64_valid(v)) {
        s.instance = vis64_instance(v);
        s.triangle = vis64_triangle(v);
    }
    return s;
}

/// Depth the export pass writes (Hi-Z input): min((q + 1) / 2^24, 1); cleared pixels -> 1.
FUSE_HOST_DEVICE inline f32 vis64_export_depth(u64 v) {
    if (!vis64_valid(v)) {
        return 1.f;
    }
    const f32 d = static_cast<f32>(vis64_depth_bits(v) + 1u) * kVis64DepthUnit;
    return d < 1.f ? d : 1.f;
}

/// Raster-target word pair (R32G32_UINT texel).
FUSE_HOST_DEVICE inline VisSample vis_raster_unpack(u32 x, u32 y) {
    VisSample s{};
    s.instance = x;
    s.triangle = x == kVisInvalid ? kVisInvalid : y;
    return s;
}

} // namespace fuse::renderer::visbuffer
