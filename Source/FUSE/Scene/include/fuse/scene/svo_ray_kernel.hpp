#pragma once

// Single-source SVO ray cast (docs/compute-kernels.md). This header is device-safe and holds the ONLY
// implementation of the octree walk, the brick payload decode and the voxel 3D-DDA: SVO::get /
// SVO::rayCast (svo.cpp), the batched CPU launches (svo_ray_cast.cpp) and the CUDA trampoline
// (kernels/svo_ray_cast.cu) all run this code. The SVO is read through `SvoView`, a POD of flat
// node / brick / payload-word spans, so the same data can live in host or device memory.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::scene {

/// Interior octree node (B3.5). Each covers a cubic region; a child slot holds the index of the
/// next interior node or, one level above the brick depth, a brick index (`SVO::kNoNode` = empty).
struct SVONode {
    u32 children[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                       0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
};

/// Leaf brick record: an 8x8x8 voxel block (smaller when `maxDepth` < 3). The payload lives in a
/// pooled word arena and takes one of four forms:
///  - Sparse:  `count` sorted (localIndex, material) pairs; sdf is implicit (+/- half a voxel).
///  - Masked:  one material shared by every written voxel plus a "written" bitmask (fill edges).
///  - Dense:   one material per voxel, a "written" bitmask and an optional per-voxel sdf plane.
///  - Uniform: every voxel written with one material (`payload` holds it) and the implicit sdf.
/// Sparse grows into Masked or Dense; any write Masked/Uniform cannot express promotes to Dense.
struct SVOBrick {
    u32 payload = 0;
    u16 count = 0; ///< sparse entry count
    u8 mode = 0;
    u8 sizeClass = 0;
};

/// One ray of a batched cast (`dir` need not be unit; `max_distance` along the normalised ray).
struct SvoRay {
    f32 origin[3] = {0.f, 0.f, 0.f};
    f32 dir[3] = {0.f, 0.f, 1.f};
    f32 max_distance = 0.f;
};

/// Result of one ray: `hit` != 0 when a solid voxel was entered at `distance`; `normal` is the face
/// the ray entered through (zero when the ray starts inside a solid voxel).
struct SvoRayHit {
    s32 voxel[3] = {0, 0, 0};
    f32 normal[3] = {0.f, 0.f, 0.f};
    f32 distance = 0.f;
    u32 hit = 0;
};

namespace svo_kernel {

/// Kernel / profiler / GPU-timestamp name.
inline constexpr const char* kName = "svo_ray_cast";
/// One ray per item; 64 rays per workgroup (CUDA block / Vulkan local_size).
inline constexpr kernel::Dim3 kWorkgroup{64u, 1u, 1u};

inline constexpr u32 kNoNode = 0xFFFFFFFFu;

// Brick payload forms (SVOBrick::mode).
inline constexpr u8 kModeSparse = 0;
inline constexpr u8 kModeDense = 1;
inline constexpr u8 kModeUniform = 2;
inline constexpr u8 kModeMasked = 3;

/// Bricks are at most 8^3 voxels (see svo.cpp): 512 bits = 8 u64 words of solid mask.
inline constexpr u32 kMaxBrickLog = 3;
inline constexpr u32 kMaxMaskWords64 = 8;
/// Deepest octree the traversal stack covers (`1 << maxDepth` voxels per axis must fit an s32).
inline constexpr u32 kMaxDepth = 30;

/// POD view of an SVO: flat arrays plus the constants every walk needs (resolved on the host).
struct SvoView {
    kernel::Span<const SVONode> nodes{};
    kernel::Span<const SVOBrick> bricks{};
    kernel::Span<const u32> pool{};
    f32 origin[3] = {0.f, 0.f, 0.f};
    f32 leaf_size = 1.f;
    u32 max_depth = 0;
    u32 brick_log = 0;
    u32 brick_depth = 0;
    u32 brick_voxels = 1;
    u32 mask_words = 1;
    u32 initialized = 0;
    u32 has_voxels = 0; ///< any solid voxel (an empty SVO is never hit)
};

FUSE_HOST_DEVICE inline bool in_bounds(const SvoView& v, s32 x, s32 y, s32 z) {
    const s32 resolution = static_cast<s32>(1u << v.max_depth);
    return x >= 0 && y >= 0 && z >= 0 && x < resolution && y < resolution && z < resolution;
}

FUSE_HOST_DEVICE inline u32 local_index(const SvoView& v, s32 x, s32 y, s32 z) {
    const u32 mask = (1u << v.brick_log) - 1u;
    return (static_cast<u32>(x) & mask) | ((static_cast<u32>(y) & mask) << v.brick_log) |
           ((static_cast<u32>(z) & mask) << (2u * v.brick_log));
}

FUSE_HOST_DEVICE inline u32 octant_of(s32 x, s32 y, s32 z, u32 mask) {
    return ((static_cast<u32>(x) & mask) ? 1u : 0u) | ((static_cast<u32>(y) & mask) ? 2u : 0u) |
           ((static_cast<u32>(z) & mask) ? 4u : 0u);
}

/// Brick holding voxel (x, y, z), or kNoNode when no voxel of that brick was ever written.
FUSE_HOST_DEVICE inline u32 find_brick(const SvoView& v, s32 x, s32 y, s32 z) {
    if (v.initialized == 0u || !in_bounds(v, x, y, z)) {
        return kNoNode;
    }
    if (v.brick_depth == 0u) {
        return 0u;
    }
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < v.brick_depth; ++depth) {
        nodeIndex = v.nodes[nodeIndex].children[octant_of(x, y, z, 1u << (v.max_depth - depth - 1u))];
        if (nodeIndex == kNoNode) {
            return kNoNode;
        }
    }
    return nodeIndex; // the last hop lands on a brick index
}

/// Binary search over sorted (localIndex, material) pairs; returns the insertion slot.
FUSE_HOST_DEVICE inline u32 sparse_lower_bound(const u32* entries, u32 count, u32 local) {
    u32 lo = 0;
    u32 hi = count;
    while (lo < hi) {
        const u32 mid = (lo + hi) >> 1u;
        if (entries[mid * 2u] < local) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/// Written flag + material of voxel `local` of `brick` (dense bricks report their stored word).
FUSE_HOST_DEVICE inline bool read_voxel(const SvoView& v, u32 brick, u32 local, u32& material) {
    const SVOBrick& b = v.bricks[brick];
    if (b.mode == kModeUniform) {
        material = b.payload;
        return true;
    }
    const u32* words = v.pool.data + b.payload;
    if (b.mode == kModeSparse) {
        const u32 slot = sparse_lower_bound(words, b.count, local);
        if (slot < b.count && words[slot * 2u] == local) {
            material = words[slot * 2u + 1u];
            return true;
        }
        material = 0u;
        return false;
    }
    if (b.mode == kModeMasked) {
        const bool written = ((words[1u + (local >> 5u)] >> (local & 31u)) & 1u) != 0u;
        material = written ? words[0] : 0u;
        return written;
    }
    material = words[local];
    return ((words[v.brick_voxels + (local >> 5u)] >> (local & 31u)) & 1u) != 0u;
}

/// Material of voxel (x, y, z); 0 when empty, out of bounds or never written.
FUSE_HOST_DEVICE inline u32 voxel_material(const SvoView& v, s32 x, s32 y, s32 z) {
    const u32 brick = find_brick(v, x, y, z);
    if (brick == kNoNode) {
        return 0u;
    }
    u32 material = 0u;
    (void)read_voxel(v, brick, local_index(v, x, y, z), material);
    return material;
}

/// Fills `mask` (one bit per voxel) with the solid voxels of a non-dense brick; true if any.
FUSE_HOST_DEVICE inline bool solid_mask(const SvoView& v, u32 brick, u64* mask) {
    const SVOBrick& b = v.bricks[brick];
    const u32 words64 = (v.brick_voxels + 63u) / 64u;
    for (u32 i = 0; i < words64; ++i) {
        mask[i] = 0ull;
    }
    bool any = false;
    if (b.mode == kModeUniform) {
        if (b.payload == 0u) {
            return false;
        }
        for (u32 i = 0; i < v.brick_voxels; ++i) {
            mask[i >> 6u] |= 1ull << (i & 63u);
        }
        return true;
    }
    const u32* words = v.pool.data + b.payload;
    if (b.mode == kModeSparse) {
        for (u32 i = 0; i < b.count; ++i) {
            if (words[i * 2u + 1u] != 0u) {
                const u32 local = words[i * 2u];
                mask[local >> 6u] |= 1ull << (local & 63u);
                any = true;
            }
        }
        return any;
    }
    if (b.mode == kModeMasked) {
        for (u32 i = 0; i < v.mask_words; ++i) {
            const u64 bits = words[1u + i];
            mask[i >> 1u] |= bits << ((i & 1u) * 32u);
            any = any || bits != 0u;
        }
        return any;
    }
    for (u32 i = 0; i < v.brick_voxels; ++i) {
        if (words[i] != 0u) {
            mask[i >> 6u] |= 1ull << (i & 63u);
            any = true;
        }
    }
    return any;
}

/// Fixed-size per-ray traversal stack: the interior nodes of the last walked root-to-brick path.
/// A later walk restarts from the deepest node whose cube still contains the new voxel instead of
/// from the root (no recursion, no heap; ~128 bytes of registers / local memory per ray).
struct TraversalStack {
    u32 node[kMaxDepth] = {};
    s32 coord[3] = {0, 0, 0}; ///< voxel the stored path was walked for
    u32 depth = 0;            ///< node[0..depth] are valid (node[0] is the root)
};

/// Walks towards voxel `c` from the deepest still-valid stack entry; returns the brick index or kNoNode
/// plus the voxel-space size of the empty cell that stopped the walk (for ray skipping).
FUSE_HOST_DEVICE inline u32 locate(const SvoView& v, const s32* c, TraversalStack& stack, u32& emptyCellSize) {
    emptyCellSize = 1u;
    if (v.brick_depth == 0u) {
        return 0u;
    }
    // Deepest stored node whose cube also contains `c`: the top `d` coordinate bits must agree.
    const u32 diff = static_cast<u32>((c[0] ^ stack.coord[0]) | (c[1] ^ stack.coord[1]) | (c[2] ^ stack.coord[2]));
    u32 depth = stack.depth;
    while (depth > 0u && (diff >> (v.max_depth - depth)) != 0u) {
        --depth;
    }
    stack.coord[0] = c[0];
    stack.coord[1] = c[1];
    stack.coord[2] = c[2];
    u32 nodeIndex = stack.node[depth];
    for (; depth < v.brick_depth; ++depth) {
        const u32 mask = 1u << (v.max_depth - depth - 1u);
        const u32 child = v.nodes[nodeIndex].children[octant_of(c[0], c[1], c[2], mask)];
        if (child == kNoNode) {
            stack.depth = depth;
            emptyCellSize = mask; // the missing child's cube, in voxels
            return kNoNode;
        }
        nodeIndex = child;
        if (depth + 1u < v.brick_depth) {
            stack.node[depth + 1u] = child;
        }
    }
    stack.depth = v.brick_depth - 1u;
    return nodeIndex;
}

FUSE_HOST_DEVICE inline int min_axis(const f32* t) {
    return t[0] < t[1] ? (t[0] < t[2] ? 0 : 2) : (t[1] < t[2] ? 1 : 2);
}

/// Exact voxel 3D-DDA of one ray that jumps over empty octree cells and empty bricks (B3.5).
/// Returns true and fills `out` on a hit; `out` is left untouched on a miss.
FUSE_HOST_DEVICE inline bool ray_cast(const SvoView& v, const SvoRay& ray, SvoRayHit& out) {
    if (v.initialized == 0u || v.has_voxels == 0u) {
        return false;
    }
    const f32 length = std::sqrt(ray.dir[0] * ray.dir[0] + ray.dir[1] * ray.dir[1] + ray.dir[2] * ray.dir[2]);
    if (length < 1e-8f) {
        return false;
    }
    const f32 invLength = 1.f / length;
    const f32 size = v.leaf_size;
    const s32 resolution = static_cast<s32>(1u << v.max_depth);
    const f32 dir[3] = {ray.dir[0] * invLength, ray.dir[1] * invLength, ray.dir[2] * invLength};
    const f32 org[3] = {ray.origin[0] - v.origin[0], ray.origin[1] - v.origin[1], ray.origin[2] - v.origin[2]};
    const f32 extent = size * static_cast<f32>(resolution);
    const f32 maxDistance = ray.max_distance;
    constexpr f32 kInf = 3.40282347e+38f; // FLT_MAX

    // Slab test against the grid bounds; remember which face the ray enters through.
    f32 tEnter = 0.f;
    f32 tExit = kInf;
    int enterAxis = -1;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-12f) {
            if (org[a] < 0.f || org[a] > extent) {
                return false;
            }
            continue;
        }
        f32 t0 = (0.f - org[a]) / dir[a];
        f32 t1 = (extent - org[a]) / dir[a];
        if (t0 > t1) {
            const f32 swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t0 > tEnter) {
            tEnter = t0;
            enterAxis = a;
        }
        tExit = std::min(tExit, t1);
    }
    if (tEnter > tExit || tEnter > maxDistance) {
        return false;
    }

    // Amanatides & Woo 3D-DDA from the entry point. Boundary crossings are evaluated in closed
    // form so that jumps over empty octree cells and bricks land exactly on the voxel grid.
    s32 cell[3] = {0, 0, 0};
    s32 step[3] = {0, 0, 0};
    f32 tMax[3] = {kInf, kInf, kInf};
    const auto nextBoundary = [&](int a) {
        if (step[a] == 0) {
            return kInf;
        }
        const s32 plane = step[a] > 0 ? cell[a] + 1 : cell[a];
        return (static_cast<f32>(plane) * size - org[a]) / dir[a];
    };
    for (int a = 0; a < 3; ++a) {
        const f32 p = org[a] + dir[a] * tEnter;
        cell[a] = std::clamp(static_cast<s32>(std::floor(p / size)), 0, resolution - 1);
        step[a] = dir[a] > 0.f ? 1 : (dir[a] < 0.f ? -1 : 0);
        tMax[a] = nextBoundary(a);
    }

    // Brick cache: consecutive voxels usually share a brick, so walk the tree once per brick, and
    // restart each walk from the traversal stack rather than the root.
    const s32 log = static_cast<s32>(v.brick_log);
    const s32 edge = 1 << log;
    TraversalStack stack{};
    u32 brick = kNoNode;
    s32 brickCoord[3] = {-1, -1, -1};
    bool brickEmpty = false;
    u64 mask[kMaxMaskWords64] = {};

    f32 t = tEnter;
    int axis = enterAxis;
    while (t <= maxDistance) {
        const s32 bc[3] = {cell[0] >> log, cell[1] >> log, cell[2] >> log};
        u32 emptySize = 1u;
        if (bc[0] != brickCoord[0] || bc[1] != brickCoord[1] || bc[2] != brickCoord[2]) {
            brick = locate(v, cell, stack, emptySize);
            if (brick != kNoNode) {
                brickCoord[0] = bc[0];
                brickCoord[1] = bc[1];
                brickCoord[2] = bc[2];
                const u8 mode = v.bricks[brick].mode;
                brickEmpty = mode == kModeUniform ? v.bricks[brick].payload == 0u
                                                  : (mode != kModeDense && !solid_mask(v, brick, mask));
            } else {
                brickCoord[0] = brickCoord[1] = brickCoord[2] = -1;
            }
        }

        if (brick != kNoNode) {
            const SVOBrick& b = v.bricks[brick];
            bool solid = false;
            if (brickEmpty) {
                emptySize = static_cast<u32>(edge);
            } else if (b.mode == kModeUniform) {
                solid = true;
            } else {
                const u32 local = local_index(v, cell[0], cell[1], cell[2]);
                solid = b.mode != kModeDense ? ((mask[local >> 6u] >> (local & 63u)) & 1ull) != 0u
                                             : v.pool[b.payload + local] != 0u;
            }
            if (solid) {
                out.voxel[0] = cell[0];
                out.voxel[1] = cell[1];
                out.voxel[2] = cell[2];
                out.distance = t;
                out.normal[0] = out.normal[1] = out.normal[2] = 0.f;
                if (axis >= 0) {
                    out.normal[axis] = dir[axis] > 0.f ? -1.f : 1.f; // face the ray entered through
                }
                out.hit = 1u;
                return true;
            }
        }

        if (emptySize == 1u) {
            axis = min_axis(tMax);
            t = tMax[axis];
            cell[axis] += step[axis];
            if (cell[axis] < 0 || cell[axis] >= resolution) {
                return false;
            }
            tMax[axis] = nextBoundary(axis);
            continue;
        }

        // Jump out of the empty aligned cube containing the current cell.
        const s32 cubeSize = static_cast<s32>(emptySize);
        s32 cubeMin[3];
        f32 faceT[3];
        for (int a = 0; a < 3; ++a) {
            cubeMin[a] = cell[a] & ~(cubeSize - 1);
            faceT[a] = step[a] == 0 ? kInf
                                    : (static_cast<f32>(step[a] > 0 ? cubeMin[a] + cubeSize : cubeMin[a]) * size -
                                       org[a]) /
                                          dir[a];
        }
        axis = min_axis(faceT);
        t = faceT[axis];
        for (int a = 0; a < 3; ++a) {
            if (a == axis || step[a] == 0) {
                continue;
            }
            const s32 inside = static_cast<s32>(std::floor((org[a] + dir[a] * t) / size));
            cell[a] = std::max(cell[a] * step[a], std::clamp(inside, cubeMin[a], cubeMin[a] + cubeSize - 1) * step[a]) *
                      step[a]; // never step backwards
            tMax[a] = nextBoundary(a);
        }
        cell[axis] = step[axis] > 0 ? cubeMin[axis] + cubeSize : cubeMin[axis] - 1;
        if (cell[axis] < 0 || cell[axis] >= resolution) {
            return false;
        }
        tMax[axis] = nextBoundary(axis);
    }
    return false;
}

/// Batched launch params: one ray in, one hit record out per item.
struct Params {
    SvoView svo{};
    kernel::Span<const SvoRay> rays{};
    kernel::Span<SvoRayHit> hits{};
};

/// Host-side validation shared by every backend (false = reject the launch).
inline bool params_valid(const Params& p) {
    return p.svo.max_depth <= kMaxDepth && p.svo.brick_log <= kMaxBrickLog && p.hits.size >= p.rays.size &&
           (p.rays.size == 0u || (p.rays.data != nullptr && p.hits.data != nullptr)) &&
           (p.svo.initialized == 0u || p.svo.bricks.data != nullptr);
}

inline kernel::KernelLaunch make_launch(u32 rayCount) {
    return kernel::KernelLaunch{kName, kernel::extent1(rayCount), kWorkgroup};
}

/// The kernel body: one ray per item; a miss writes a cleared record.
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        SvoRayHit result{};
        (void)ray_cast(p.svo, p.rays[idx.linear], result);
        p.hits[idx.linear] = result;
    }
};

} // namespace svo_kernel
} // namespace fuse::scene
