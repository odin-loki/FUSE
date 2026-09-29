#pragma once

// Single-source SVO ray cast (docs/compute-kernels.md). This header is device-safe and holds the ONLY
// implementation of the octree walk, the brick payload decode and the voxel 3D-DDA: SVO::get /
// SVO::rayCast (svo.cpp), the batched CPU launches (svo_ray_cast.cpp) and the CUDA trampoline
// (kernels/svo_ray_cast.cu) all run this code. The SVO is read through `SvoView`, a POD of flat
// node / brick / payload-word spans, so the same data can live in host or device memory. The ray walk
// also accepts a view of the linearised ray-walk layout (SVO::rayLayout: 8-byte PackedNode records in
// depth-first order, repacked bricks / payload) — the layout the CUDA kernel reads; it is the same tree,
// so both views give bit-identical hits.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <bit>
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
/// One ray per item; 128 rays per workgroup (CUDA block / Vulkan local_size). A 64-thread block caps
/// sm_86 at 16 resident blocks = 32 of 48 warps; 128 lets registers, not the block limit, set occupancy.
inline constexpr kernel::Dim3 kWorkgroup{128u, 1u, 1u};

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

/// Linearised interior node of the ray-walk layout (SVO::rayLayout): 8 bytes instead of SVONode's 32.
/// The existing children of a node are stored contiguously in octant order, so child `o` is
/// `first + popcount(mask & ((1 << o) - 1))` when bit `o` of `mask` is set. Nodes are laid out depth
/// first (a node's child block right after its parent's siblings' subtrees start), so a ray's walk and
/// its neighbours touch nearby cache lines. At the brick level `first` indexes the packed brick array.
struct PackedNode {
    u32 first = 0;
    u32 mask = 0;
};

/// POD view of an SVO: flat arrays plus the constants every walk needs (resolved on the host).
/// `packed` empty: the SVO's own arrays (`nodes`, `bricks`, `pool`). `packed` set: a ray-walk layout
/// (SVO::rayLayout) — `bricks` / `pool` are its repacked copies and `nodes` is unused; only the ray
/// walk (locate / ray_cast) accepts such a view.
struct SvoView {
    kernel::Span<const SVONode> nodes{};
    kernel::Span<const SVOBrick> bricks{};
    kernel::Span<const u32> pool{};
    kernel::Span<const PackedNode> packed{};
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
/// `node` has no default initializer: a walk only reads entries it wrote (node[0] = root is set by the
/// caller), so a GPU ray does not zero 30 words of local memory up front. `TraversalStack s{}` zeroes all.
struct TraversalStack {
    u32 node[kMaxDepth];
    s32 coord[3] = {0, 0, 0}; ///< voxel the stored path was walked for
    u32 depth = 0;            ///< node[0..depth] are valid (node[0] is the root)
    /// Packed views: the record of node[brick_depth - 1] (valid whenever depth == brick_depth - 1),
    /// so moving to a sibling brick cell reads no memory at all.
    PackedNode leaf{};
};

FUSE_HOST_DEVICE inline u32 popcount32(u32 x) {
#if defined(__CUDA_ARCH__)
    return static_cast<u32>(__popc(x));
#else
    return static_cast<u32>(std::popcount(x));
#endif
}

/// Number of significant bits of `x` (0 for 0): std::bit_width.
FUSE_HOST_DEVICE inline u32 bit_width32(u32 x) {
#if defined(__CUDA_ARCH__)
    return 32u - static_cast<u32>(__clz(static_cast<int>(x)));
#else
    return static_cast<u32>(std::bit_width(x));
#endif
}

/// Child `octant` of interior node `node` (a node index, or a brick index one level above the bricks),
/// kNoNode when absent — from the packed layout when the view carries one, else from SVONode.
FUSE_HOST_DEVICE inline u32 child_of(const SvoView& v, u32 node, u32 octant) {
    if (v.packed.size != 0u) {
        const PackedNode n = v.packed[node];
        const u32 bit = 1u << octant;
        return (n.mask & bit) != 0u ? n.first + popcount32(n.mask & (bit - 1u)) : kNoNode;
    }
    return v.nodes[node].children[octant];
}

/// Walks towards voxel `c` from the deepest still-valid stack entry; returns the brick index or kNoNode
/// plus the voxel-space size of the empty cell that stopped the walk (for ray skipping).
FUSE_HOST_DEVICE inline u32 locate(const SvoView& v, const s32* c, TraversalStack& stack, u32& emptyCellSize) {
    emptyCellSize = 1u;
    if (v.brick_depth == 0u) {
        return 0u;
    }
    // Deepest stored node whose cube also contains `c`: the top `d` coordinate bits must agree.
    // (diff >> (max_depth - d)) == 0  <=>  d <= max_depth - bit_width(diff).
    const u32 diff = static_cast<u32>((c[0] ^ stack.coord[0]) | (c[1] ^ stack.coord[1]) | (c[2] ^ stack.coord[2]));
    const u32 width = bit_width32(diff);
    const u32 shared = width >= v.max_depth ? 0u : v.max_depth - width;
    const u32 depth0 = stack.depth < shared ? stack.depth : shared;
    u32 depth = depth0;
    stack.coord[0] = c[0];
    stack.coord[1] = c[1];
    stack.coord[2] = c[2];
    const u32 last = v.brick_depth - 1u;
    if (v.packed.size != 0u) {
        // Packed layout: one 8-byte record per level, the brick-level parent cached in the stack.
        PackedNode n = depth == last ? stack.leaf : v.packed[stack.node[depth]];
        for (;; ++depth) {
            const u32 cube = 1u << (v.max_depth - depth - 1u);
            const u32 bit = 1u << octant_of(c[0], c[1], c[2], cube);
            if (depth == last) {
                stack.leaf = n;
            }
            if ((n.mask & bit) == 0u) {
                stack.depth = depth;
                emptyCellSize = cube; // the missing child's cube, in voxels
                return kNoNode;
            }
            const u32 child = n.first + popcount32(n.mask & (bit - 1u));
            if (depth == last) {
                stack.depth = last;
                return child; // a brick index
            }
            stack.node[depth + 1u] = child;
            n = v.packed[child];
        }
    }
    u32 nodeIndex = stack.node[depth];
    for (; depth < v.brick_depth; ++depth) {
        const u32 mask = 1u << (v.max_depth - depth - 1u);
        const u32 child = child_of(v, nodeIndex, octant_of(c[0], c[1], c[2], mask));
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

/// Occupancy summary of the brick a ray is inside, decoded once when the ray enters the brick and then
/// kept in registers: the payload form, where its words live and a 64-bit mask of the brick's 2x2x2
/// voxel cells that hold a solid voxel (bit `cx | cy << 2 | cz << 4`). Sparse bricks get an exact cell
/// mask, so a ray crosses a brick holding a few voxels in a handful of 4^3 / 2^3 jumps instead of
/// one step per voxel; masked / dense bricks keep an all-ones mask (voxel by voxel, as before).
struct BrickState {
    u64 cells = 0;
    u32 words = 0; ///< payload word offset (uniform: the material)
    u32 count = 0; ///< sparse entry count
    u32 mode = 0;
};

/// Bit of the 2x2x2 cell holding brick-local voxel (lx, ly, lz) in BrickState::cells.
FUSE_HOST_DEVICE inline u32 cell2_bit(u32 lx, u32 ly, u32 lz) {
    return (lx >> 1u) | ((ly >> 1u) << 2u) | ((lz >> 1u) << 4u);
}

/// Cells of the 4x4x4 sub-block holding brick-local voxel (lx, ly, lz) (8-voxel bricks only).
FUSE_HOST_DEVICE inline u64 cell4_bits(u32 lx, u32 ly, u32 lz) {
    return 0x0000000000330033ull << (((lx >> 2u) << 1u) | ((ly >> 2u) << 3u) | ((lz >> 2u) << 5u));
}

/// Decodes brick `brick` for the traversal; false when it holds no solid voxel (same emptiness rule as
/// solid_mask: sparse / dense materials != 0, masked written bits, uniform payload != 0).
FUSE_HOST_DEVICE inline bool decode_brick(const SvoView& v, u32 brick, BrickState& s) {
    const SVOBrick b = v.bricks[brick];
    s.mode = b.mode;
    s.words = b.payload;
    s.count = b.count;
    s.cells = ~0ull;
    if (b.mode == kModeUniform) {
        return b.payload != 0u;
    }
    const u32* words = v.pool.data + b.payload;
    if (b.mode == kModeSparse) {
        const u32 mask = (1u << v.brick_log) - 1u;
        u64 cells = 0ull;
        for (u32 i = 0; i < b.count; ++i) {
            if (words[i * 2u + 1u] != 0u) {
                const u32 local = words[i * 2u];
                cells |= 1ull << cell2_bit(local & mask, (local >> v.brick_log) & mask, local >> (2u * v.brick_log));
            }
        }
        s.cells = cells;
        return cells != 0ull;
    }
    if (b.mode == kModeMasked) {
        u32 any = 0u;
        for (u32 i = 0; i < v.mask_words; ++i) {
            any |= words[1u + i];
        }
        return any != 0u;
    }
    return true; // dense: stepped voxel by voxel
}

/// Solid test of brick-local voxel `local` of a decoded (non-empty) brick.
FUSE_HOST_DEVICE inline bool brick_voxel_solid(const SvoView& v, const BrickState& s, u32 local) {
    if (s.mode == kModeUniform) {
        return true;
    }
    const u32* words = v.pool.data + s.words;
    if (s.mode == kModeSparse) {
        const u32 slot = sparse_lower_bound(words, s.count, local);
        return slot < s.count && words[slot * 2u] == local && words[slot * 2u + 1u] != 0u;
    }
    if (s.mode == kModeMasked) {
        return ((words[1u + (local >> 5u)] >> (local & 31u)) & 1u) != 0u;
    }
    return words[local] != 0u;
}

/// Exact voxel 3D-DDA of one ray over the SVO (B3.5), written for the GPU as much as for the CPU:
///  - one loop, no recursion, no heap: the state is the current voxel, the ray time and the entry axis;
///  - every iteration finds the largest empty aligned cube around the current voxel -- a missing octree
///    child (16^3 .. root), an empty brick (8^3), an empty 4^3 sub-block or 2^3 cell of a sparse brick
///    (from the register-resident cell mask), or a single empty voxel -- and leaves it in one step:
///    exit-plane times in voxel units against a per-ray reciprocal direction, no divisions;
///  - the octree walk restarts from the deepest node of the last path that still contains the voxel
///    (TraversalStack), and happens only when the ray enters a new brick;
///  - the payload pool is read only to decode a brick on entry and to test voxels of occupied cells.
/// The hit record is computed exactly as before: the entry plane of the hit voxel in closed form
/// ((plane * size - origin) / dir, or the grid-entry time for the first voxel) and its face normal, so
/// the result depends only on which voxel is hit and through which face.
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
    if (tEnter > tExit || !(tEnter <= maxDistance)) {
        return false;
    }

    // Entry voxel; from here on the walk runs in voxel units: position(tv) = ov + dir * tv. The small
    // per-axis arrays are only ever indexed by unrolled loop counters (never by `axis`), so nvcc keeps
    // them in registers; the traversal stack is the only per-ray local memory.
    s32 cell[3] = {0, 0, 0};
    s32 step[3] = {0, 0, 0};
    f32 ov[3] = {0.f, 0.f, 0.f};
    f32 inv[3] = {0.f, 0.f, 0.f};
    for (int a = 0; a < 3; ++a) {
        const f32 p = org[a] + dir[a] * tEnter;
        cell[a] = std::clamp(static_cast<s32>(std::floor(p / size)), 0, resolution - 1);
        step[a] = dir[a] > 0.f ? 1 : (dir[a] < 0.f ? -1 : 0);
        ov[a] = org[a] / size;
        if (step[a] != 0) {
            inv[a] = 1.f / dir[a];
            if (!(std::fabs(inv[a]) <= kInf)) {
                inv[a] = step[a] > 0 ? kInf : -kInf; // denormal direction: never NaN in (plane - ov) * inv
            }
        }
    }
    // Walk-time limit with slack: the exact `distance <= max_distance` test is made on the hit itself.
    const f32 tvLimit = maxDistance / size * 1.0001f + 1.f;

    const u32 log = v.brick_log;
    const s32 edge = 1 << log;
    const u32 localMask = static_cast<u32>(edge) - 1u;
    TraversalStack stack;
    stack.node[0] = 0u; // root; depth 0 and coord are default-initialised
    if (v.packed.size != 0u) {
        stack.leaf = v.packed[0]; // node[depth] when depth == brick_depth - 1 == 0
    }
    BrickState brick{};
    s32 brickCoord[3] = {-1, -1, -1};
    int axis = enterAxis;
    bool moved = false;

    for (;;) {
        // Size of the empty aligned cube around `cell` (0 = the voxel is solid).
        s32 empty = 0;
        if ((cell[0] >> log) != brickCoord[0] || (cell[1] >> log) != brickCoord[1] ||
            (cell[2] >> log) != brickCoord[2]) {
            u32 cellSize = 1u;
            const u32 index = locate(v, cell, stack, cellSize);
            if (index == kNoNode) {
                empty = static_cast<s32>(cellSize);
                brickCoord[0] = -1;
            } else if (!decode_brick(v, index, brick)) {
                empty = edge;
                brickCoord[0] = -1;
            } else {
                brickCoord[0] = cell[0] >> log;
                brickCoord[1] = cell[1] >> log;
                brickCoord[2] = cell[2] >> log;
            }
        }
        if (empty == 0 && brick.mode != kModeUniform) {
            const u32 lx = static_cast<u32>(cell[0]) & localMask;
            const u32 ly = static_cast<u32>(cell[1]) & localMask;
            const u32 lz = static_cast<u32>(cell[2]) & localMask;
            if (edge >= 8 && (brick.cells & cell4_bits(lx, ly, lz)) == 0ull) {
                empty = 4;
            } else if (edge >= 2 && ((brick.cells >> cell2_bit(lx, ly, lz)) & 1ull) == 0ull) {
                empty = 2;
            } else if (!brick_voxel_solid(v, brick, lx | (ly << log) | (lz << (2u * log)))) {
                empty = 1;
            }
        }

        if (empty == 0) {
            // Entry plane of the hit voxel in closed form (bit-identical to a voxel-by-voxel DDA).
            f32 distance = tEnter;
            if (moved) {
                const s32 c = axis == 0 ? cell[0] : (axis == 1 ? cell[1] : cell[2]);
                const s32 s = axis == 0 ? step[0] : (axis == 1 ? step[1] : step[2]);
                const f32 o = axis == 0 ? org[0] : (axis == 1 ? org[1] : org[2]);
                const f32 d = axis == 0 ? dir[0] : (axis == 1 ? dir[1] : dir[2]);
                distance = (static_cast<f32>(s > 0 ? c : c + 1) * size - o) / d;
            }
            if (!(distance <= maxDistance)) {
                return false;
            }
            out.voxel[0] = cell[0];
            out.voxel[1] = cell[1];
            out.voxel[2] = cell[2];
            out.distance = distance;
            // Face the ray entered through (none when it starts inside a solid voxel).
            out.normal[0] = axis == 0 ? (dir[0] > 0.f ? -1.f : 1.f) : 0.f;
            out.normal[1] = axis == 1 ? (dir[1] > 0.f ? -1.f : 1.f) : 0.f;
            out.normal[2] = axis == 2 ? (dir[2] > 0.f ? -1.f : 1.f) : 0.f;
            out.hit = 1u;
            return true;
        }

        // Leave the empty aligned cube through its nearest exit face: the exit axis moves to the
        // neighbouring cube, the others to the voxel the ray is in at the exit time (clamped to the
        // cube, never backwards). Every axis is computed and then selected, so no array is indexed by
        // `axis` (that would put the state in local memory on the GPU).
        s32 cubeMin[3];
        f32 exitT[3];
        for (int a = 0; a < 3; ++a) {
            cubeMin[a] = cell[a] & ~(empty - 1);
            exitT[a] = step[a] == 0 ? kInf
                                    : (static_cast<f32>(step[a] > 0 ? cubeMin[a] + empty : cubeMin[a]) - ov[a]) * inv[a];
        }
        axis = min_axis(exitT);
        const f32 tv = axis == 0 ? exitT[0] : (axis == 1 ? exitT[1] : exitT[2]);
        if (!(tv <= tvLimit)) {
            return false;
        }
        for (int a = 0; a < 3; ++a) {
            s32 across = cell[a];
            if (empty > 1 && step[a] != 0) {
                const s32 inside = static_cast<s32>(std::floor(ov[a] + dir[a] * tv));
                across = std::max(cell[a] * step[a], std::clamp(inside, cubeMin[a], cubeMin[a] + empty - 1) * step[a]) *
                         step[a]; // never step backwards
            }
            const s32 beyond = step[a] > 0 ? cubeMin[a] + empty : cubeMin[a] - 1;
            cell[a] = a == axis ? beyond : across;
        }
        if (cell[0] < 0 || cell[0] >= resolution || cell[1] < 0 || cell[1] >= resolution || cell[2] < 0 ||
            cell[2] >= resolution) {
            return false;
        }
        moved = true;
    }
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
