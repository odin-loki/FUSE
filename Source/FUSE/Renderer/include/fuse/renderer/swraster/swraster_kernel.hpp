#pragma once

// WP-5.4 compute software rasteriser: the CPU reference as single-source kernels
// (docs/compute-kernels.md). shaders/swraster/swraster_common.{glsl,slang} and the kernels
// swraster_{classify,raster}.{comp,slang} are line-for-line twins (same operations in the same
// order); fuse_rp_swraster checks the GPU against these kernels BIT FOR BIT.
//
// Why bit-exact is possible: after the vertex transform (the WP-1.4 fuse_vis_clip: correctly rounded
// f32 multiplies and adds, nothing contracted: GLSL `precise`, Slang -fp-mode precise, no FMA on the
// host) every step is integer arithmetic. In particular the perspective divide is NOT a float divide
// (Vulkan allows 2.5 ulp for those): x / w is evaluated exactly from the two floats' mantissas and
// exponents with 64-bit integer division (ratio_round / ratio_floor_z), so both sides get the same
// correctly rounded fixed-point result.
//
// Rasterisation rules (documented for WP-5.4's "edge rules" acceptance item):
//   * Vertices snap to 1/256 px (kSwSubpixelBits = 8): sx = round(x / w * width * 128) + width * 128,
//     round = floor(v + 1/2) of the EXACT quotient (Vulkan viewport transform, y down, no y flip).
//   * Coverage is sampled at pixel centres (px + 1/2, py + 1/2) with integer edge functions
//     E_ab(p) = (b.x - a.x)(p.y - a.y) - (b.y - a.y)(p.x - a.x) over the snapped vertices, after
//     swapping v1 / v2 so that the doubled area is positive (both windings rasterise: the WP-1.4
//     pipelines are two-sided). Zero-area triangles are skipped.
//   * Top-left fill rule (D3D / common Vulkan convention, y down): a centre exactly on an edge (E == 0)
//     is covered iff the edge is a LEFT edge (dy < 0 in the normalised winding: the interior lies to
//     its right) or a TOP edge (dy == 0 and dx > 0: horizontal, interior below). Adjacent triangles
//     sharing an edge therefore cover every centre on it exactly once (fuse_rp_swraster_fill).
//   * Depth: z / w per vertex as floor(z / w * 2^32) (exact), interpolated at the centre with the
//     integer barycentrics: z = floor(sum(E_i z_i) / sum(E_i)) (64-bit), word depth = z >> 8, i.e.
//     floor(depth * 2^24) as vis64_quantize_depth; the WP-1.4 64-bit word (depth 24 | instance 20 |
//     triangle 20, vis_format.hpp) is written with 64-bit atomicMin.
// How that differs from a hardware rasteriser (the SW-vs-HW gate measures it, see swraster.hpp):
// the HW snaps a float x / w (not the exact quotient) and interpolates depth in float, so a vertex can
// land one sub-pixel away and a depth one 2^-24 quantum away; pixels whose centre lies within that
// distance of an edge, or of a second surface, may resolve differently. The HW tie-break on exact
// edge hits is implementation-defined in Vulkan; Lavapipe's results are consistent with top-left
// (fuse_rp_swraster counts exact edge hits and how many of them resolve differently: <= 1 / frame).
// SW depth is the plane through the SNAPPED vertices (always inside the triangle's true depth
// range); Lavapipe's is a float plane through the exact ones, so on sliver triangles the same pixel's
// depth differs by up to a few hundred 2^-24 quanta.
//
// Cluster classification (classify_cluster): the meshlet's object-space AABB (WP-1.2: a box around
// the DECODED vertices) is transformed corner by corner with the same vertex transform. Culled when
// all 8 corners are outside one clip plane, or when their exact snapped rect (+1 px) holds no pixel
// centre of the target; HardwareClip when a corner is at / behind the near plane (w <= 0 or z < 0) or
// beyond the far plane (z > w) - the HW path clips, the SW path cannot; HardwareExtent when the rect
// is larger than maxClusterPixels (<= kSwMaxClusterPixels) in x or y; otherwise Software when
// (ForceSoftware or) the rect area per triangle is at most threshold^2 (the estimated triangle size:
// sqrt(rectW * rectH / triangles) <= threshold px), else HardwareSize. The decision is integer except
// the corner transform, so the GPU equals this kernel exactly.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/swraster/swraster_types.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>

#include <cstring>

#if !defined(__CUDA_ARCH__)
#include <atomic>
#endif

namespace fuse::renderer::swraster::sw_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuMeshlet;
using gpu_scene::GpuTransform;
using visbuffer::decode_kernel::Clip;

inline constexpr const char* kClassifyName = "swraster_classify";
inline constexpr const char* kRasterName = "swraster_raster";

// --- exact fixed-point projection -------------------------------------------------------------------

FUSE_HOST_DEVICE inline u32 f32_bits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

/// round(num * scale / den) (round half up: floor(q + 1/2)) of the EXACT quotient, for a positive
/// normal `den`, a finite `num` and scale < 2^22. False when an input is outside that domain or
/// |result| > kSwCoordLimit.
FUSE_HOST_DEVICE inline bool ratio_round(f32 num, f32 den, u32 scale, s64& out) {
    out = 0;
    const u32 nb = f32_bits(num);
    const u32 db = f32_bits(den);
    const u32 ne = (nb >> 23u) & 0xFFu;
    const u32 de = (db >> 23u) & 0xFFu;
    if ((db >> 31u) != 0u || de == 0u || de == 0xFFu || ne == 0xFFu) {
        return false; // den <= 0, denormal, inf / NaN; num inf / NaN
    }
    if ((nb & 0x7FFFFFFFu) == 0u) {
        return true; // +-0
    }
    const u64 mn = ne == 0u ? static_cast<u64>(nb & 0x7FFFFFu) : static_cast<u64>((nb & 0x7FFFFFu) | 0x800000u);
    const s32 en = ne == 0u ? -149 : static_cast<s32>(ne) - 150;
    const u64 md = static_cast<u64>((db & 0x7FFFFFu) | 0x800000u);
    const s32 ed = static_cast<s32>(de) - 150;
    const s32 k = en - ed;
    u64 numer = mn * static_cast<u64>(scale) * 2u; // < 2^47
    u64 denom = md;                                // [2^23, 2^24)
    if (k >= 0) {
        if (k > 15) {
            return false; // |q| >= 2^15 * mn * scale / md: far outside any representable target
        }
        numer <<= static_cast<u32>(k); // < 2^62
    } else {
        if (-k > 38) {
            return true; // |q| < 2^47 / 2^(23 + 39) < 1/2: rounds to 0 either way
        }
        denom <<= static_cast<u32>(-k); // < 2^62
    }
    const bool negative = (nb >> 31u) != 0u;
    const u64 q = negative ? (numer + denom - 1u) / (denom * 2u) : (numer + denom) / (denom * 2u);
    if (q > static_cast<u64>(kSwCoordLimit)) {
        return false;
    }
    out = negative ? -static_cast<s64>(q) : static_cast<s64>(q);
    return true;
}

/// floor(z / w * 2^32) clamped to 2^32 - 1, for 0 <= z <= w, w positive normal (EXACT quotient).
FUSE_HOST_DEVICE inline u32 ratio_floor_z(f32 z, f32 w) {
    const u32 zb = f32_bits(z);
    const u32 wb = f32_bits(w);
    if ((zb & 0x7FFFFFFFu) == 0u) {
        return 0u;
    }
    const u32 ze = (zb >> 23u) & 0xFFu;
    const u64 mz = ze == 0u ? static_cast<u64>(zb & 0x7FFFFFu) : static_cast<u64>((zb & 0x7FFFFFu) | 0x800000u);
    const s32 ez = ze == 0u ? -149 : static_cast<s32>(ze) - 150;
    const u64 mw = static_cast<u64>((wb & 0x7FFFFFu) | 0x800000u);
    const s32 ew = static_cast<s32>((wb >> 23u) & 0xFFu) - 150;
    const s32 k = ez - ew + 32;
    u64 q = 0;
    if (k >= 0) {
        q = (mz << static_cast<u32>(k)) / mw; // z <= w: k <= 32, mz << k < 2^57
    } else if (-k < 24) {
        q = mz / (mw << static_cast<u32>(-k));
    }
    return q > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(q);
}

/// A vertex in fixed point: x, y in sub-pixels (absolute), z = floor(z / w * 2^32).
struct SwVertex {
    s32 x = 0;
    s32 y = 0;
    u32 z = 0;
    u32 valid = 0; ///< 1 when strictly in front (w > 0), 0 <= z <= w and representable
};

FUSE_HOST_DEVICE inline SwVertex project_vertex(const Clip& c, u32 width, u32 height) {
    SwVertex v{};
    if (!(c.w > 0.f) || !(c.z >= 0.f) || !(c.z <= c.w)) {
        return v;
    }
    s64 rx = 0;
    s64 ry = 0;
    if (!ratio_round(c.x, c.w, width << 7u, rx) || !ratio_round(c.y, c.w, height << 7u, ry)) {
        return v;
    }
    v.x = static_cast<s32>(rx + static_cast<s64>(width << 7u));
    v.y = static_cast<s32>(ry + static_cast<s64>(height << 7u));
    v.z = ratio_floor_z(c.z, c.w);
    v.valid = 1u;
    return v;
}

// --- triangle setup and scan ------------------------------------------------------------------------

/// Top-left rule for the edge a -> b of a positively oriented triangle (see the file comment).
FUSE_HOST_DEVICE inline s32 edge_bias(s32 dx, s32 dy) { return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1; }

/// ceil((v - 128) / 256) and floor((v - 128) / 256): the pixel-centre range of a sub-pixel interval.
FUSE_HOST_DEVICE inline s32 first_centre(s32 v) { return (v + (kSwSubpixelHalf - 1)) >> kSwSubpixelBits; }
FUSE_HOST_DEVICE inline s32 last_centre(s32 v) { return (v - kSwSubpixelHalf) >> kSwSubpixelBits; }

FUSE_HOST_DEVICE inline u64 pack_word(u32 depth24, u32 instance, u32 triangle) {
    return (static_cast<u64>(depth24) << visbuffer::kVis64DepthShift) |
           (static_cast<u64>(instance & visbuffer::kVis64IdMask) << visbuffer::kVis64InstanceShift) |
           static_cast<u64>(triangle & visbuffer::kVis64IdMask);
}

/// Rasterises one triangle of snapped vertices (spans bounded by kSwMaxSpanSubpixels); calls
/// write(pixelIndex, word) for every covered pixel centre of the width x height target. Returns the
/// number of covered centres.
template <typename Write>
FUSE_HOST_DEVICE inline u32 raster_triangle(SwVertex v0, SwVertex v1, SwVertex v2, u32 width, u32 height, u32 instance,
                                            u32 triangle, Write&& write) {
    s32 area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0) {
        return 0u;
    }
    if (area < 0) {
        const SwVertex t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }
    const s32 minX = v0.x < v1.x ? (v0.x < v2.x ? v0.x : v2.x) : (v1.x < v2.x ? v1.x : v2.x);
    const s32 maxX = v0.x > v1.x ? (v0.x > v2.x ? v0.x : v2.x) : (v1.x > v2.x ? v1.x : v2.x);
    const s32 minY = v0.y < v1.y ? (v0.y < v2.y ? v0.y : v2.y) : (v1.y < v2.y ? v1.y : v2.y);
    const s32 maxY = v0.y > v1.y ? (v0.y > v2.y ? v0.y : v2.y) : (v1.y > v2.y ? v1.y : v2.y);
    s32 px0 = first_centre(minX);
    s32 px1 = last_centre(maxX);
    s32 py0 = first_centre(minY);
    s32 py1 = last_centre(maxY);
    px0 = px0 > 0 ? px0 : 0;
    py0 = py0 > 0 ? py0 : 0;
    px1 = px1 < static_cast<s32>(width) - 1 ? px1 : static_cast<s32>(width) - 1;
    py1 = py1 < static_cast<s32>(height) - 1 ? py1 : static_cast<s32>(height) - 1;
    if (px0 > px1 || py0 > py1) {
        return 0u;
    }
    // Edge i is opposite vertex i: E0 = v1 -> v2, E1 = v2 -> v0, E2 = v0 -> v1.
    const s32 dx0 = v2.x - v1.x;
    const s32 dy0 = v2.y - v1.y;
    const s32 dx1 = v0.x - v2.x;
    const s32 dy1 = v0.y - v2.y;
    const s32 dx2 = v1.x - v0.x;
    const s32 dy2 = v1.y - v0.y;
    const s32 b0 = edge_bias(dx0, dy0);
    const s32 b1 = edge_bias(dx1, dy1);
    const s32 b2 = edge_bias(dx2, dy2);
    const s32 ox = px0 * kSwSubpixelOne + kSwSubpixelHalf;
    const s32 oy = py0 * kSwSubpixelOne + kSwSubpixelHalf;
    s32 r0 = dx0 * (oy - v1.y) - dy0 * (ox - v1.x);
    s32 r1 = dx1 * (oy - v2.y) - dy1 * (ox - v2.x);
    s32 r2 = dx2 * (oy - v0.y) - dy2 * (ox - v0.x);
    const s32 sx0 = -dy0 * kSwSubpixelOne;
    const s32 sx1 = -dy1 * kSwSubpixelOne;
    const s32 sx2 = -dy2 * kSwSubpixelOne;
    const s32 sy0 = dx0 * kSwSubpixelOne;
    const s32 sy1 = dx1 * kSwSubpixelOne;
    const s32 sy2 = dx2 * kSwSubpixelOne;
    u32 covered = 0;
    for (s32 py = py0; py <= py1; ++py) {
        s32 e0 = r0;
        s32 e1 = r1;
        s32 e2 = r2;
        for (s32 px = px0; px <= px1; ++px) {
            if (e0 + b0 >= 0 && e1 + b1 >= 0 && e2 + b2 >= 0) {
                const u64 num = static_cast<u64>(e0) * v0.z + static_cast<u64>(e1) * v1.z + static_cast<u64>(e2) * v2.z;
                const u64 z = num / static_cast<u64>(area);
                write(static_cast<u32>(py) * width + static_cast<u32>(px), pack_word(static_cast<u32>(z >> 8u), instance, triangle));
                ++covered;
            }
            e0 += sx0;
            e1 += sx1;
            e2 += sx2;
        }
        r0 += sy0;
        r1 += sy1;
        r2 += sy2;
    }
    return covered;
}

// --- cluster classification -------------------------------------------------------------------------

FUSE_HOST_DEVICE inline u32 meshlet_vertex_count(const GpuMeshlet& m) { return m.counts & 0xFFu; }
FUSE_HOST_DEVICE inline u32 meshlet_triangle_count(const GpuMeshlet& m) { return (m.counts >> 8u) & 0xFFu; }

/// SwResult of one meshlet of `instance` (see the file comment).
FUSE_HOST_DEVICE inline u32 classify_cluster(const GpuMeshlet& m, const GpuTransform& xf, const SwRasterConstants& c) {
    const u32 vertices = meshlet_vertex_count(m);
    const u32 triangles = meshlet_triangle_count(m);
    if (vertices > kSwMaxVertices || triangles > kSwMaxTriangles) {
        return kSwResultOversize;
    }
    Clip corner[8];
    u32 allOut = 0x3Fu;
    bool clip = false;
    for (u32 k = 0; k < 8u; ++k) {
        f32 p[3];
        p[0] = (k & 1u) != 0u ? m.aabbMax[0] : m.aabbMin[0];
        p[1] = (k & 2u) != 0u ? m.aabbMax[1] : m.aabbMin[1];
        p[2] = (k & 4u) != 0u ? m.aabbMax[2] : m.aabbMin[2];
        const Clip q = visbuffer::decode_kernel::clip_position(xf, c.viewProj, p);
        corner[k] = q;
        const u32 out = (q.x < -q.w ? 1u : 0u) | (q.x > q.w ? 2u : 0u) | (q.y < -q.w ? 4u : 0u) | (q.y > q.w ? 8u : 0u) |
                        (q.z < 0.f ? 16u : 0u) | (q.z > q.w ? 32u : 0u);
        allOut &= out;
        if (!(q.w > 0.f) || !(q.z >= 0.f) || !(q.z <= q.w)) {
            clip = true;
        }
    }
    if (allOut != 0u) {
        return kSwResultCulled;
    }
    if (c.mode == kSwModeForceHardware) {
        return kSwResultHardwareForced;
    }
    if (clip) {
        return kSwResultHardwareClip;
    }
    s32 minX = 0x7FFFFFFF;
    s32 minY = 0x7FFFFFFF;
    s32 maxX = -0x7FFFFFFF;
    s32 maxY = -0x7FFFFFFF;
    for (u32 k = 0; k < 8u; ++k) {
        const SwVertex v = project_vertex(corner[k], c.width, c.height);
        if (v.valid == 0u) {
            return kSwResultHardwareClip;
        }
        minX = v.x < minX ? v.x : minX;
        maxX = v.x > maxX ? v.x : maxX;
        minY = v.y < minY ? v.y : minY;
        maxY = v.y > maxY ? v.y : maxY;
    }
    // No pixel centre within 1 px of the rect: nothing to draw.
    const s32 lastX = static_cast<s32>(c.width - 1u) * kSwSubpixelOne + kSwSubpixelHalf;
    const s32 lastY = static_cast<s32>(c.height - 1u) * kSwSubpixelOne + kSwSubpixelHalf;
    if (maxX + kSwSubpixelOne < kSwSubpixelHalf || maxY + kSwSubpixelOne < kSwSubpixelHalf || minX - kSwSubpixelOne > lastX ||
        minY - kSwSubpixelOne > lastY) {
        return kSwResultCulled;
    }
    const u32 rectW = static_cast<u32>(maxX - minX);
    const u32 rectH = static_cast<u32>(maxY - minY);
    if (rectW > c.maxClusterExtent || rectH > c.maxClusterExtent) {
        return kSwResultHardwareExtent;
    }
    if (c.mode == kSwModeForceSoftware) {
        return kSwResultSoftware;
    }
    const u64 thr = c.triangleThreshold;
    return static_cast<u64>(rectW) * rectH <= thr * thr * triangles ? kSwResultSoftware : kSwResultHardwareSize;
}

// --- CPU scene view -----------------------------------------------------------------------------------

/// One mesh's meshlet streams on the CPU (the GpuMesh BDA streams of the GPU scene).
struct SwMeshGeometry {
    kernel::Span<const GpuMeshlet> meshlets;
    kernel::Span<const u32> vertices;  ///< MVRT
    kernel::Span<const u32> triangles; ///< MTRI (i0 | i1 << 8 | i2 << 16)
    kernel::Span<const u16> vpos;      ///< VPOS, 4 per vertex
    u32 vertexCount = 0;
};

struct SwSceneView {
    kernel::Span<const GpuInstance> instances;
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuMesh> meshes;
    kernel::Span<const SwMeshGeometry> geometry; ///< per mesh index
};

/// Clip position of meshlet-local vertex `v` (the GPU's MVRT -> VPOS -> fuse_vis_clip); false when an
/// index is out of range (never for a valid cooked mesh).
FUSE_HOST_DEVICE inline bool meshlet_vertex_clip(const GpuMesh& mesh, const SwMeshGeometry& g, const GpuMeshlet& m, u32 v,
                                                 const GpuTransform& xf, const f32 viewProj[16], Clip& out) {
    if (m.vertexOffset + v >= g.vertices.size) {
        return false;
    }
    const u32 vertex = static_cast<u32>(static_cast<s32>(g.vertices[m.vertexOffset + v]) + mesh.vertexOffset);
    if (vertex >= g.vertexCount || static_cast<u64>(vertex) * 4u + 3u >= g.vpos.size) {
        return false;
    }
    f32 p[3];
    visbuffer::decode_kernel::mesh_position(mesh, g.vpos.data, vertex, p);
    out = visbuffer::decode_kernel::clip_position(xf, viewProj, p);
    return true;
}

// --- host / device atomics ----------------------------------------------------------------------------

FUSE_HOST_DEVICE inline void global_atomic_min_u64(u64* address, u64 value) {
#if defined(__CUDA_ARCH__)
    atomicMin(reinterpret_cast<unsigned long long*>(address), static_cast<unsigned long long>(value));
#else
    std::atomic_ref<u64> ref(*address);
    u64 current = ref.load(std::memory_order_relaxed);
    while (value < current && !ref.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
#endif
}

FUSE_HOST_DEVICE inline u32 global_atomic_add_u32(u32* address, u32 value) {
#if defined(__CUDA_ARCH__)
    return atomicAdd(address, value);
#else
    return std::atomic_ref<u32>(*address).fetch_add(value, std::memory_order_relaxed);
#endif
}

// --- classify kernel (one item per (record, lane)) ------------------------------------------------------

struct ClassifyParams {
    SwSceneView scene{};
    kernel::Span<const SwGroup> groups; ///< the region's records (GPU read-back)
    SwRasterConstants constants{};
    kernel::Span<u32> results; ///< groups.size * kSwGroupSize
};

struct ClassifyKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ClassifyParams& p) const {
        const u32 i = idx.global.x;
        const u32 g = i / kSwGroupSize;
        const u32 lane = i % kSwGroupSize;
        if (g >= p.groups.size) {
            return;
        }
        const SwGroup rec = p.groups[g];
        u32 result = kSwResultNone;
        if (rec.instance < p.scene.instances.size) {
            const GpuInstance& inst = p.scene.instances[rec.instance];
            if (inst.mesh < p.scene.meshes.size && inst.mesh < p.scene.geometry.size) {
                const GpuMesh& mesh = p.scene.meshes[inst.mesh];
                const u32 m = rec.firstMeshlet + lane;
                if (m < mesh.meshletCount && m < p.scene.geometry[inst.mesh].meshlets.size) {
                    result = classify_cluster(p.scene.geometry[inst.mesh].meshlets[m], p.scene.transforms[rec.instance],
                                              p.constants);
                }
            }
        }
        p.results[i] = result;
    }
};

inline kernel::KernelLaunch make_classify_launch(u32 groups) {
    return kernel::KernelLaunch{kClassifyName, kernel::extent1(groups * kSwGroupSize), {kSwGroupSize, 1u, 1u}};
}

// --- raster kernel (one workgroup of kSwRasterThreads per SW cluster) --------------------------------

struct RasterParams {
    SwSceneView scene{};
    kernel::Span<const SwCluster> clusters; ///< the SW list (GPU read-back, any order)
    SwRasterConstants constants{};
    kernel::Span<u64> target;               ///< width * height words, cleared to kVis64Clear by the caller
    kernel::Span<SwCluster> demoted;        ///< clusters moved to the HW list (order undefined)
    u32* demotedCount = nullptr;
    u32* triangles = nullptr;               ///< triangles set up (kSwCountSwTriangles)
};

/// Scratch layout (u32): x[64] | y[64] | z[64] | valid[64] | minX maxX minY maxY invalid.
inline constexpr u32 kScratchX = 0u;
inline constexpr u32 kScratchY = kSwMaxVertices;
inline constexpr u32 kScratchZ = kSwMaxVertices * 2u;
inline constexpr u32 kScratchValid = kSwMaxVertices * 3u;
inline constexpr u32 kScratchBounds = kSwMaxVertices * 4u;
inline constexpr u32 kScratchInvalid = kScratchBounds + 4u;
inline constexpr u32 kScratchWords = kScratchInvalid + 4u;

FUSE_HOST_DEVICE inline s32 as_s32(u32 v) { return static_cast<s32>(v); }
FUSE_HOST_DEVICE inline u32 as_u32(s32 v) { return static_cast<u32>(v); }

struct RasterKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = kScratchWords;
    static constexpr u32 kPhases = 3u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const RasterParams& p) const {
        u32* s = wg.scratch;
        const u32 lane = idx.local.x;
        const u32 ci = idx.group.x;
        if (ci >= p.clusters.size) {
            return;
        }
        const SwCluster cl = p.clusters[ci];
        const GpuInstance& inst = p.scene.instances[cl.instance];
        const GpuMesh& mesh = p.scene.meshes[inst.mesh];
        const SwMeshGeometry& g = p.scene.geometry[inst.mesh];
        const GpuMeshlet& m = g.meshlets[cl.meshlet];
        const u32 vertices = meshlet_vertex_count(m);
        const u32 triangles = meshlet_triangle_count(m);
        const SwRasterConstants& c = p.constants;
        if (wg.phase == 0u) {
            if (lane == 0u) {
                s[kScratchBounds + 0u] = as_u32(0x7FFFFFFF);
                s[kScratchBounds + 1u] = as_u32(-0x7FFFFFFF);
                s[kScratchBounds + 2u] = as_u32(0x7FFFFFFF);
                s[kScratchBounds + 3u] = as_u32(-0x7FFFFFFF);
                s[kScratchInvalid] = 0u;
            }
            return;
        }
        if (wg.phase == 1u) {
            if (lane < vertices) {
                Clip cc{};
                SwVertex v{};
                if (meshlet_vertex_clip(mesh, g, m, lane, p.scene.transforms[cl.instance], c.viewProj, cc)) {
                    v = project_vertex(cc, c.width, c.height);
                }
                s[kScratchX + lane] = as_u32(v.x);
                s[kScratchY + lane] = as_u32(v.y);
                s[kScratchZ + lane] = v.z;
                s[kScratchValid + lane] = v.valid;
                if (v.valid == 0u) {
                    s[kScratchInvalid] = 1u;
                } else {
                    s[kScratchBounds + 0u] = as_u32(v.x < as_s32(s[kScratchBounds + 0u]) ? v.x : as_s32(s[kScratchBounds + 0u]));
                    s[kScratchBounds + 1u] = as_u32(v.x > as_s32(s[kScratchBounds + 1u]) ? v.x : as_s32(s[kScratchBounds + 1u]));
                    s[kScratchBounds + 2u] = as_u32(v.y < as_s32(s[kScratchBounds + 2u]) ? v.y : as_s32(s[kScratchBounds + 2u]));
                    s[kScratchBounds + 3u] = as_u32(v.y > as_s32(s[kScratchBounds + 3u]) ? v.y : as_s32(s[kScratchBounds + 3u]));
                }
            }
            return;
        }
        // Phase 2: demote (whole cluster) or rasterise.
        const s32 spanX = as_s32(s[kScratchBounds + 1u]) - as_s32(s[kScratchBounds + 0u]);
        const s32 spanY = as_s32(s[kScratchBounds + 3u]) - as_s32(s[kScratchBounds + 2u]);
        if (s[kScratchInvalid] != 0u || spanX > kSwMaxSpanSubpixels || spanY > kSwMaxSpanSubpixels) {
            if (lane == 0u && p.demotedCount != nullptr) {
                const u32 at = global_atomic_add_u32(p.demotedCount, 1u);
                if (at < p.demoted.size) {
                    p.demoted[at] = cl;
                }
            }
            return;
        }
        if (lane == 0u && p.triangles != nullptr) {
            global_atomic_add_u32(p.triangles, triangles);
        }
        if ((c.flags & kSwFlagSkipSoftware) != 0u) {
            return;
        }
        for (u32 t = lane; t < triangles; t += kSwRasterThreads) {
            const u32 packed = g.triangles[m.triangleOffset + t];
            SwVertex v[3];
            for (u32 k = 0; k < 3u; ++k) {
                const u32 li = (packed >> (8u * k)) & 0xFFu;
                v[k].x = as_s32(s[kScratchX + li]);
                v[k].y = as_s32(s[kScratchY + li]);
                v[k].z = s[kScratchZ + li];
                v[k].valid = 1u;
            }
            raster_triangle(v[0], v[1], v[2], c.width, c.height, cl.instance, m.triangleOffset + t,
                            [&](u32 pixel, u64 word) { global_atomic_min_u64(&p.target[pixel], word); });
        }
    }
};

inline kernel::KernelLaunch make_raster_launch(u32 clusters) {
    return kernel::KernelLaunch{kRasterName, kernel::extent1(clusters * kSwRasterThreads), {kSwRasterThreads, 1u, 1u}};
}

} // namespace fuse::renderer::swraster::sw_kernel
