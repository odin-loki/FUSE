#pragma once

// Single-source broadphase kernels (docs/compute-kernels.md): the spatial-hash broadphase as a
// sequence of data-parallel launches that run unchanged on CpuReference, CpuParallel and CUDA
// (kernels/physics_broadphase.cu). Every body here is FUSE_HOST_DEVICE and device-safe (POD params,
// raw pointers, no allocation). Host orchestration lives in broadphase_kernels.hpp / .cpp.
//
//   physics_broadphase_count      item / shape: hash cells the shape's AABB covers
//   physics_scan (+ _add)         workgroup reduce-then-scan (block scan, block sums, add back)
//   physics_broadphase_keys       item / shape: (cell key, body) entries at the scanned offset
//   physics_broadphase_sort_hist  workgroup / tile: 4-bit digit histogram, digit-major
//   physics_broadphase_sort       workgroup / tile: stable in-tile ranking + scatter
//   physics_broadphase_runs       item / entry: cell-boundary flags   (count)
//   physics_broadphase_run_starts item / entry: cell start offsets    (write at scanned index)
//   physics_broadphase_cells      item / cell: sort + unique occupants, pair count
//   physics_broadphase_pairs      item / cell: packed canonical pair keys at the scanned offset
//   physics_broadphase_pair_flags / physics_broadphase_pair_unique: dedupe of the sorted keys
//
// Order is deterministic by construction: every output position comes from an exclusive scan
// (count -> scan -> write) or a stable radix sort, never from atomic arrival order, so all
// backends (and the legacy CPU path in spatial_hash.cpp) produce the same pair list.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::broadphase_kernel {

using namespace fuse::physics::broadphase; // CellRange*, cell / hash helpers, SpatialHashParams

inline constexpr const char* kCountName = "physics_broadphase_count";
inline constexpr const char* kKeysName = "physics_broadphase_keys";
inline constexpr const char* kSortHistName = "physics_broadphase_sort_hist";
inline constexpr const char* kSortName = "physics_broadphase_sort";
inline constexpr const char* kScanName = "physics_scan";
inline constexpr const char* kScanAddName = "physics_scan_add";
inline constexpr const char* kRunsName = "physics_broadphase_runs";
inline constexpr const char* kRunStartsName = "physics_broadphase_run_starts";
inline constexpr const char* kCellsName = "physics_broadphase_cells";
inline constexpr const char* kPairsName = "physics_broadphase_pairs";
inline constexpr const char* kPairFlagsName = "physics_broadphase_pair_flags";
inline constexpr const char* kPairUniqueName = "physics_broadphase_pair_unique";

/// Item kernels: 64 threads per workgroup.
inline constexpr kernel::Dim3 kItemWorkgroup{64u, 1u, 1u};

// ---------------------------------------------------------------------------------------------
// Shape -> hash cells (shared with the legacy CPU path: spatial_hash.cpp calls shapeCells too).
// ---------------------------------------------------------------------------------------------

/// Flat view of the body / shape SoA plus the normalized hash parameters.
struct ShapeView {
    const vec3* positions = nullptr;
    const quat* orientations = nullptr;
    u32 orientationCount = 0;
    u32 bodyCount = 0;
    const u32* shapeTypes = nullptr;
    const vec3* shapeParams = nullptr;
    const u32* shapeBodies = nullptr;
    u32 shapeCount = 0;
    f32 cellSize = 1.f;
    u32 tableSize = 1;
    u32 maxSpan = 0;
    u32 maxOccupancy = 0;
    u32 use2D = 0;
};

/// Cell range a shape occupies; `insert` is false when the shape is skipped (body out of range,
/// empty range or over the occupancy budget) — the shapeCellInsertRejectReason rules.
struct ShapeCells {
    CellRange3 range3{};
    CellRange2 range2{};
    u32 body = 0;
    bool insert = false;
};

FUSE_HOST_DEVICE inline ShapeCells shapeCells(const ShapeView& v, u32 shape) {
    ShapeCells out{};
    out.body = shape < v.shapeCount ? v.shapeBodies[shape] : 0u;
    if (out.body >= v.bodyCount) {
        return out;
    }
    const vec3 position = v.positions[out.body];
    const f32 cellSize = clampCellSize(v.cellSize);
    const bool box = static_cast<CollisionShapeType>(v.shapeTypes[shape]) == CollisionShapeType::Box;
    vec3 halfExtents{};
    f32 radius = 0.f;
    if (box) {
        halfExtents = out.body < v.orientationCount
                          ? orientedBoxHalfExtents(v.orientations[out.body], v.shapeParams[shape])
                          : v.shapeParams[shape];
    } else {
        radius = v.shapeParams[shape].x;
    }
    if (v.use2D != 0u) {
        out.range2 = box ? cellRangeFromAabb2D(aabbFromBox(position, halfExtents), cellSize, v.maxSpan)
                         : cellRangeFromSphere2D(vec2{position.x, position.y}, radius, cellSize, v.maxSpan);
        out.insert = cellOccupancyRejectReason(out.range2, v.maxOccupancy) == CellOccupancyRejectReason::None;
    } else {
        out.range3 = box ? cellRangeFromBox(position, halfExtents, cellSize, v.maxSpan)
                         : cellRangeFromSphere(position, radius, cellSize, v.maxSpan);
        out.insert = cellOccupancyRejectReason(out.range3, v.maxOccupancy) == CellOccupancyRejectReason::None;
    }
    return out;
}

/// Number of (key, body) entries shapeCellsVisit emits for `cells`.
FUSE_HOST_DEVICE inline u32 shapeCellCount(const ShapeView& v, const ShapeCells& cells) {
    if (!cells.insert) {
        return 0u;
    }
    return v.use2D != 0u ? estimateCellOccupancyCount(cells.range2) : estimateCellOccupancyCount(cells.range3);
}

/// Calls visit(key, body) for every occupied cell, z/y/x (2D: y/x) outer to inner.
template <typename Visit>
FUSE_HOST_DEVICE inline void shapeCellsVisit(const ShapeView& v, const ShapeCells& cells, Visit&& visit) {
    if (!cells.insert) {
        return;
    }
    const u32 tableSize = clampTableSize(v.tableSize);
    if (v.use2D != 0u) {
        for (s32 cy = cells.range2.minCell.y; cy <= cells.range2.maxCell.y; ++cy) {
            for (s32 cx = cells.range2.minCell.x; cx <= cells.range2.maxCell.x; ++cx) {
                visit(spatialHash2D(cx, cy, tableSize), cells.body);
            }
        }
        return;
    }
    for (s32 cz = cells.range3.minCell.z; cz <= cells.range3.maxCell.z; ++cz) {
        for (s32 cy = cells.range3.minCell.y; cy <= cells.range3.maxCell.y; ++cy) {
            for (s32 cx = cells.range3.minCell.x; cx <= cells.range3.maxCell.x; ++cx) {
                visit(spatialHash(cx, cy, cz, tableSize), cells.body);
            }
        }
    }
}

/// Host-only: view of the SoA vectors + normalized params (spans stay valid while the SoA is unchanged).
inline ShapeView makeShapeView(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes,
                               const SpatialHashParams& params, bool use2D) {
    ShapeView v{};
    v.positions = bodies.positions.data();
    v.orientations = bodies.orientations.data();
    v.orientationCount = static_cast<u32>(bodies.orientations.size());
    v.bodyCount = bodies.count();
    v.shapeTypes = shapes.types.data();
    v.shapeParams = shapes.params.data();
    v.shapeBodies = shapes.bodyIndices.data();
    v.shapeCount = shapes.count();
    v.cellSize = params.cellSize;
    v.tableSize = params.tableSize;
    v.maxSpan = params.maxCellSpanPerAxis;
    v.maxOccupancy = params.maxCellOccupancy;
    v.use2D = use2D ? 1u : 0u;
    return v;
}

struct CellCountParams {
    ShapeView view{};
    u32* counts = nullptr; ///< per shape
};

struct CellCountKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CellCountParams& p) const {
        p.counts[idx.linear] = shapeCellCount(p.view, shapeCells(p.view, idx.linear));
    }
};

struct CellKeysParams {
    ShapeView view{};
    const u32* offsets = nullptr; ///< exclusive scan of the counts
    u32* keys = nullptr;
    u32* bodies = nullptr;
};

struct CellKeysKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CellKeysParams& p) const {
        u32 write = p.offsets[idx.linear];
        u32* keys = p.keys;
        u32* bodies = p.bodies;
        shapeCellsVisit(p.view, shapeCells(p.view, idx.linear), [&](u32 key, u32 body) {
            keys[write] = key;
            bodies[write] = body;
            ++write;
        });
    }
};

// ---------------------------------------------------------------------------------------------
// Exclusive scan (reduce-then-scan; the pattern of fuse_rhi GpuRadixSort radix_scan / scan_add).
// ---------------------------------------------------------------------------------------------

inline constexpr u32 kScanWorkgroup = 256u;
inline constexpr u32 kScanItems = 4u;
inline constexpr u32 kScanChunk = kScanWorkgroup * kScanItems; ///< elements per workgroup
inline constexpr u32 kScanRake = 16u;                          ///< raking threads (16 x 16 sums)

struct ScanParams {
    u32* data = nullptr;
    u32 count = 0;
    u32* blockSums = nullptr; ///< one total per 1024-element chunk
};

/// Exclusive scan of each 1024-element chunk in place; the chunk total goes to blockSums[chunk].
/// Launch with grid = chunks * 256 (every thread in range).
struct ScanBlocksKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = kScanChunk + kScanWorkgroup + kScanRake; // values | sums | rake
    static constexpr u32 kPhases = 4u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const ScanParams& p) const {
        u32* values = wg.scratch;
        u32* sums = wg.scratch + kScanChunk;
        u32* rake = sums + kScanWorkgroup;
        const u32 t = idx.local_linear;
        const u32 base = idx.group.x * kScanChunk + t * kScanItems;
        switch (wg.phase) {
        case 0: {
            u32 sum = 0u;
            for (u32 i = 0; i < kScanItems; ++i) {
                const u32 v = base + i < p.count ? p.data[base + i] : 0u;
                values[t * kScanItems + i] = v;
                sum += v;
            }
            sums[t] = sum;
            break;
        }
        case 1:
            if (t < kScanRake) {
                u32 running = 0u;
                for (u32 j = 0; j < kScanWorkgroup / kScanRake; ++j) {
                    const u32 v = sums[t * (kScanWorkgroup / kScanRake) + j];
                    sums[t * (kScanWorkgroup / kScanRake) + j] = running;
                    running += v;
                }
                rake[t] = running;
            }
            break;
        case 2:
            if (t == 0u) {
                u32 running = 0u;
                for (u32 j = 0; j < kScanRake; ++j) {
                    const u32 v = rake[j];
                    rake[j] = running;
                    running += v;
                }
                if (p.blockSums != nullptr) {
                    p.blockSums[idx.group.x] = running;
                }
            }
            break;
        default: {
            u32 running = sums[t] + rake[t / (kScanWorkgroup / kScanRake)];
            for (u32 i = 0; i < kScanItems; ++i) {
                if (base + i < p.count) {
                    p.data[base + i] = running;
                }
                running += values[t * kScanItems + i];
            }
            break;
        }
        }
    }
};

struct ScanAddParams {
    u32* data = nullptr;
    const u32* blockSums = nullptr; ///< scanned chunk totals
};

/// data[i] += blockSums[i / 1024] (grid = count).
struct ScanAddKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ScanAddParams& p) const {
        p.data[idx.linear] += p.blockSums[idx.linear / kScanChunk];
    }
};

// ---------------------------------------------------------------------------------------------
// Stable LSD radix sort of u32 keys (+ optional u32 values): the GpuRadixSort structure — tile
// histograms written digit-major, one exclusive scan giving every (digit, tile) its global
// offset, then a stable in-tile scatter — sized for the 16 KiB portable workgroup scratch:
// 4-bit digits, 1024-element tiles (256 threads x 4), raked 16-element ranking chunks.
// Launch both kernels with grid = tiles * kSortThreads, workgroup = kSortThreads.
// ---------------------------------------------------------------------------------------------

inline constexpr u32 kSortThreads = 256u;
inline constexpr u32 kSortItems = 4u;                        ///< elements per thread
inline constexpr u32 kSortTile = kSortThreads * kSortItems;  ///< 1024 elements per tile
inline constexpr u32 kDigitBits = 4u;
inline constexpr u32 kBuckets = 1u << kDigitBits;
inline constexpr u32 kRankChunk = 16u;                       ///< elements per ranking chunk
inline constexpr u32 kRankChunks = kSortTile / kRankChunk;   ///< 64 chunks (raking threads) per tile
static_assert(kBuckets * kRankChunks == kSortTile, "counter array is tile-sized");
static_assert(kRankChunks <= kSortThreads, "one raking thread per chunk");

struct RadixParams {
    const u32* keysIn = nullptr;
    const u32* valsIn = nullptr; ///< may be null (keys-only sort)
    u32* keysOut = nullptr;
    u32* valsOut = nullptr;
    u32 count = 0;
    u32 shift = 0;
    u32 numTiles = 0;
    u32* hist = nullptr; ///< kBuckets * numTiles, digit-major: hist[digit * numTiles + tile]
};

FUSE_HOST_DEVICE inline u32 radixDigit(u32 key, u32 shift) {
    return (key >> shift) & (kBuckets - 1u);
}

/// Element k of thread t: strided (k * 256 + t) so consecutive threads touch consecutive keys.
FUSE_HOST_DEVICE inline u32 sortElement(u32 t, u32 k) {
    return k * kSortThreads + t;
}

/// Per-tile digit histogram.
struct RadixHistogramKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = kBuckets;
    static constexpr u32 kPhases = 3u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const RadixParams& p) const {
        const u32 t = idx.local_linear;
        const u32 tileBase = idx.group.x * kSortTile;
        switch (wg.phase) {
        case 0:
            if (t < kBuckets) {
                wg.scratch[t] = 0u;
            }
            break;
        case 1:
            for (u32 k = 0; k < kSortItems; ++k) {
                const u32 i = tileBase + sortElement(t, k);
                if (i < p.count) {
                    kernel::scratch_atomic_add(&wg.scratch[radixDigit(p.keysIn[i], p.shift)], 1u);
                }
            }
            break;
        default:
            if (t < kBuckets) {
                p.hist[t * p.numTiles + idx.group.x] = wg.scratch[t];
            }
            break;
        }
    }
};

/// Stable scatter of one tile. The tile is cut into 64 chunks of 16 consecutive elements; one raking
/// thread per chunk walks its chunk in order, counting (digit, chunk) occurrences and giving each
/// element its rank among the same-digit elements before it in the chunk. An exclusive scan of each
/// digit's chunk counters then gives the same-digit elements of earlier chunks, so
/// output = scanned global offset of (digit, tile) + chunk prefix + in-chunk rank: equal keys keep
/// their input order across chunks, tiles and passes.
struct RadixScatterKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = kSortTile * 3u + kBuckets; // digits | counters | ranks | bases
    static constexpr u32 kPhases = 4u;
    static constexpr u32 kPadding = 0xFFu; ///< digit of out-of-range elements (matches no bucket)

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const RadixParams& p) const {
        u32* digits = wg.scratch;
        u32* counters = digits + kSortTile; ///< [digit * kRankChunks + chunk]
        u32* ranks = counters + kSortTile;  ///< in-chunk rank of each element
        u32* bases = ranks + kSortTile;
        const u32 t = idx.local_linear;
        const u32 tileBase = idx.group.x * kSortTile;
        switch (wg.phase) {
        case 0:
            for (u32 k = 0; k < kSortItems; ++k) {
                const u32 e = sortElement(t, k);
                digits[e] = tileBase + e < p.count ? radixDigit(p.keysIn[tileBase + e], p.shift) : kPadding;
                counters[e] = 0u;
            }
            if (t < kBuckets) {
                bases[t] = p.hist[t * p.numTiles + idx.group.x];
            }
            break;
        case 1:
            if (t < kRankChunks) {
                for (u32 j = 0; j < kRankChunk; ++j) {
                    const u32 e = t * kRankChunk + j;
                    const u32 d = digits[e];
                    if (d != kPadding) {
                        ranks[e] = counters[d * kRankChunks + t]++;
                    }
                }
            }
            break;
        case 2:
            if (t < kBuckets) {
                u32 running = 0u;
                for (u32 c = 0; c < kRankChunks; ++c) {
                    const u32 n = counters[t * kRankChunks + c];
                    counters[t * kRankChunks + c] = running;
                    running += n;
                }
            }
            break;
        default:
            for (u32 k = 0; k < kSortItems; ++k) {
                const u32 e = sortElement(t, k);
                const u32 d = digits[e];
                if (d == kPadding) {
                    continue;
                }
                const u32 rank = bases[d] + counters[d * kRankChunks + e / kRankChunk] + ranks[e];
                p.keysOut[rank] = p.keysIn[tileBase + e];
                if (p.valsIn != nullptr) {
                    p.valsOut[rank] = p.valsIn[tileBase + e];
                }
            }
            break;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Runs of equal keys (count -> scan -> write compaction).
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline bool runBoundary(const u32* keys, u32 i) {
    return i == 0u || keys[i] != keys[i - 1u];
}

struct BoundaryFlagParams {
    const u32* keys = nullptr;
    u32* flags = nullptr;
};

/// flags[i] = 1 where a run of equal keys starts (grid = count).
struct BoundaryFlagKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BoundaryFlagParams& p) const {
        p.flags[idx.linear] = runBoundary(p.keys, idx.linear) ? 1u : 0u;
    }
};

struct BoundaryWriteParams {
    const u32* keys = nullptr;
    const u32* runIndex = nullptr; ///< exclusive scan of the boundary flags
    u32* starts = nullptr;
};

/// starts[run] = first index of the run (grid = count).
struct BoundaryWriteKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BoundaryWriteParams& p) const {
        if (runBoundary(p.keys, idx.linear)) {
            p.starts[p.runIndex[idx.linear]] = idx.linear;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Per-cell pair generation.
// ---------------------------------------------------------------------------------------------

/// Ascending in-place sort for a cell's occupants: insertion sort for short runs, heapsort otherwise
/// (no recursion, no allocation). Any correct sort gives the same result for u32 values.
FUSE_HOST_DEVICE inline void sortU32(u32* v, u32 n) {
    if (n <= 24u) {
        for (u32 i = 1; i < n; ++i) {
            const u32 x = v[i];
            u32 j = i;
            while (j > 0u && v[j - 1u] > x) {
                v[j] = v[j - 1u];
                --j;
            }
            v[j] = x;
        }
        return;
    }
    const auto siftDown = [v](u32 root, u32 end) {
        while (true) {
            u32 child = 2u * root + 1u;
            if (child >= end) {
                return;
            }
            if (child + 1u < end && v[child] < v[child + 1u]) {
                ++child;
            }
            if (v[root] >= v[child]) {
                return;
            }
            const u32 tmp = v[root];
            v[root] = v[child];
            v[child] = tmp;
            root = child;
        }
    };
    for (u32 start = n / 2u; start-- > 0u;) {
        siftDown(start, n);
    }
    for (u32 end = n - 1u; end > 0u; --end) {
        const u32 tmp = v[0];
        v[0] = v[end];
        v[end] = tmp;
        siftDown(0u, end);
    }
}

struct CellParams {
    const u32* runStarts = nullptr; ///< runCount + 1 offsets into bodies
    u32* bodies = nullptr;          ///< occupants grouped by cell (sorted by key)
    u32* uniqueCounts = nullptr;    ///< per run
    u32* pairCounts = nullptr;      ///< per run
};

/// Sorts + uniques one cell's occupants in place; pair count u (u - 1) / 2 (grid = runs).
struct CellKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CellParams& p) const {
        const u32 start = p.runStarts[idx.linear];
        const u32 n = p.runStarts[idx.linear + 1u] - start;
        u32* v = p.bodies + start;
        u32 unique = n;
        if (n > 1u) {
            sortU32(v, n);
            unique = 1u;
            for (u32 i = 1; i < n; ++i) {
                if (v[i] != v[unique - 1u]) {
                    v[unique++] = v[i];
                }
            }
        }
        p.uniqueCounts[idx.linear] = unique;
        p.pairCounts[idx.linear] =
            cellPairGenRejectReason(unique) == CellPairGenRejectReason::None ? estimateCellPairCount(unique) : 0u;
    }
};

struct PairParams {
    const u32* runStarts = nullptr;
    const u32* bodies = nullptr;
    const u32* uniqueCounts = nullptr;
    const u32* pairOffsets = nullptr; ///< exclusive scan of the pair counts
    u32 bits = 16;                    ///< body index bits: key = (lo << bits) | hi
    u32* pairKeys = nullptr;
};

/// Writes one cell's canonical pairs (lo < hi, lexicographic) at its scanned offset (grid = runs).
struct PairKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PairParams& p) const {
        const u32 u = p.uniqueCounts[idx.linear];
        if (u < 2u) {
            return;
        }
        const u32* v = p.bodies + p.runStarts[idx.linear];
        u32 write = p.pairOffsets[idx.linear];
        for (u32 i = 0; i < u; ++i) {
            for (u32 j = i + 1u; j < u; ++j) {
                p.pairKeys[write++] = (v[i] << p.bits) | v[j];
            }
        }
    }
};

/// Bits needed for body indices < bodyCount (at least 1) — the packing dedupeBuffer uses.
FUSE_HOST_DEVICE inline u32 bodyIndexBits(u32 bodyCount) {
    const u32 maxBody = bodyCount > 0u ? bodyCount - 1u : 0u;
    u32 bits = 1u;
    while (bits < 32u && (maxBody >> bits) != 0u) {
        ++bits;
    }
    return bits;
}

} // namespace fuse::physics::broadphase_kernel
