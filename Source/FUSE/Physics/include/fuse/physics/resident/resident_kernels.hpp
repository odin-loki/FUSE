#pragma once

// Single-source kernels of the resident physics pipeline (docs/compute-kernels.md): the device-count
// variants of the broadphase stages, the device narrowphase and the coloured constraint solve. Every
// body is FUSE_HOST_DEVICE, reads plain pointers and allocates nothing, so the same launch sequence
// (resident_physics.cpp) runs on CpuReference / CpuParallel over host memory and on CUDA over device
// memory (kernels/physics_resident.cu).
//
// Resident = no host round trip between stages. A stage whose size is data dependent (entries, cells,
// cell pairs, unique pairs, contacts) is launched over a host-known *capacity* and reads the live count
// from device memory (the total an exclusive scan left in its level storage). Items past the live count
// write zeros (so the next scan over the capacity is unchanged) or sort sentinels (so a stable radix
// sort over the capacity keeps the live keys first, in order). A capacity that is too small never writes
// out of bounds: the stage clips, the status kernel records the needed size, and the host grows the
// buffers and runs again (resident_physics.hpp).
//
//   Broadphase  physics_resident_bp_*  (count, scan, keys, pad, sort, runs, run starts, cells, pairs,
//               plane pairs, pad, sort, unique flags, scan, unique write, status): the pair list equals
//               runBroadphaseIntoBuffer / runBroadphaseKernels (sorted, unique, plane pairs merged).
//   Narrowphase physics_resident_np_*  (detect into a per-pair slot, scan, compact): the manifolds equal
//               the CPU narrowphase per pair (narrowphasePairContact or, with a margin, collidePairs).
//   Solver      physics_resident_solve_color / _serial: one launch per colour (and the serial overflow
//               list) per iteration, the per-constraint solve ported from constraint_accumulation.cpp.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::resident {

namespace np = fuse::physics::narrowphase;

inline constexpr const char* kBpCountName = "physics_resident_bp_count";
inline constexpr const char* kBpKeysName = "physics_resident_bp_keys";
inline constexpr const char* kBpPadName = "physics_resident_bp_pad";
inline constexpr const char* kBpRunsName = "physics_resident_bp_runs";
inline constexpr const char* kBpRunStartsName = "physics_resident_bp_run_starts";
inline constexpr const char* kBpCellsName = "physics_resident_bp_cells";
inline constexpr const char* kBpPairsName = "physics_resident_bp_pairs";
inline constexpr const char* kBpPlanePairsName = "physics_resident_bp_plane_pairs";
inline constexpr const char* kBpUniqueFlagsName = "physics_resident_bp_unique_flags";
inline constexpr const char* kBpUniqueWriteName = "physics_resident_bp_unique_write";
inline constexpr const char* kStatusName = "physics_resident_status";
inline constexpr const char* kNpDetectName = "physics_resident_np_detect";
inline constexpr const char* kNpCompactName = "physics_resident_np_compact";
inline constexpr const char* kSolveColorName = "physics_resident_solve_color";
inline constexpr const char* kSolveSerialName = "physics_resident_solve_serial";

inline constexpr kernel::Dim3 kItemWorkgroup{64u, 1u, 1u};

/// min(*total + extra, capacity): the live item count of a capacity-sized launch.
FUSE_HOST_DEVICE inline u32 liveCount(const u32* total, u32 extra, u32 capacity) {
    const u32 t = (total != nullptr ? *total : 0u) + extra;
    return t < capacity ? t : capacity;
}

/// Largest key a keyBits-bit sort orders: padding sorts after every live key (stable, so a live key
/// equal to it stays in front).
FUSE_HOST_DEVICE inline u32 sortSentinel(u32 keyBits) {
    return keyBits >= 32u ? 0xFFFFFFFFu : ((1u << keyBits) - 1u);
}

/// Counters the pipeline leaves in device memory (read back at the edge, resident_physics.hpp).
struct Status {
    u32 entries = 0;    ///< (cell, body) entries
    u32 cells = 0;      ///< occupied cells (runs of equal keys)
    u32 cellPairs = 0;  ///< pairs emitted per cell before the dedupe
    u32 pairSlots = 0;  ///< cell pairs + plane slots (sorted together)
    u32 pairs = 0;      ///< unique candidate pairs
    u32 contacts = 0;   ///< narrowphase manifolds
    u32 overflow = 0;   ///< bit 0 entries, bit 1 pair slots, bit 2 contacts: grow and run again
    u32 clipped = 0;    ///< writes dropped by a capacity guard (diagnostic)
};

inline constexpr u32 kOverflowEntries = 1u << 0u;
inline constexpr u32 kOverflowPairs = 1u << 1u;
inline constexpr u32 kOverflowContacts = 1u << 2u;

// ---------------------------------------------------------------------------------------------
// Broadphase (device counts). Shape -> cells is broadphase_kernel::shapeCells (the CPU path's code).
// ---------------------------------------------------------------------------------------------

struct KeysParams {
    broadphase_kernel::ShapeView view{};
    const u32* offsets = nullptr; ///< exclusive scan of the per-shape cell counts
    u32* keys = nullptr;
    u32* bodies = nullptr;
    u32 capacity = 0;
    u32* clipped = nullptr;
};

/// (cell key, body) entries of one shape at its scanned offset; writes past `capacity` are dropped
/// (the status kernel reports the overflow). Grid = shapes.
struct KeysKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const KeysParams& p) const {
        u32 write = p.offsets[idx.linear];
        u32 dropped = 0u;
        u32* keys = p.keys;
        u32* bodies = p.bodies;
        const u32 capacity = p.capacity;
        broadphase_kernel::shapeCellsVisit(p.view, broadphase_kernel::shapeCells(p.view, idx.linear),
                                           [&](u32 key, u32 body) {
                                               if (write < capacity) {
                                                   keys[write] = key;
                                                   bodies[write] = body;
                                               } else {
                                                   ++dropped;
                                               }
                                               ++write;
                                           });
        if (dropped != 0u) {
            kernel::global_atomic_add(p.clipped, dropped);
        }
    }
};

struct PadParams {
    u32* keys = nullptr;
    u32* values = nullptr; ///< may be null
    const u32* total = nullptr;
    u32 extra = 0;         ///< added to *total (host-known slots appended after the counted ones)
    u32 capacity = 0;
    u32 keySentinel = 0;
    u32 valueSentinel = 0;
};

/// keys[i] = sentinel for i in [live, capacity) (grid = capacity).
struct PadKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PadParams& p) const {
        if (idx.linear < liveCount(p.total, p.extra, p.capacity)) {
            return;
        }
        p.keys[idx.linear] = p.keySentinel;
        if (p.values != nullptr) {
            p.values[idx.linear] = p.valueSentinel;
        }
    }
};

struct RunFlagParams {
    const u32* keys = nullptr;
    const u32* total = nullptr;
    u32 capacity = 0;
    u32* flags = nullptr;
};

/// flags[i] = 1 where a run of equal keys starts, 0 past the live count (grid = capacity).
struct RunFlagKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const RunFlagParams& p) const {
        const u32 i = idx.linear;
        p.flags[i] = i < liveCount(p.total, 0u, p.capacity) && broadphase_kernel::runBoundary(p.keys, i) ? 1u : 0u;
    }
};

struct RunStartParams {
    const u32* keys = nullptr;
    const u32* entryTotal = nullptr;
    const u32* runIndex = nullptr; ///< scanned run flags
    const u32* runTotal = nullptr;
    u32 capacity = 0;
    u32* starts = nullptr; ///< capacity + 1
};

/// starts[run] = first entry of the run; item 0 also writes starts[runs] = entries (grid = capacity).
struct RunStartKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const RunStartParams& p) const {
        const u32 i = idx.linear;
        const u32 entries = liveCount(p.entryTotal, 0u, p.capacity);
        if (i < entries && broadphase_kernel::runBoundary(p.keys, i)) {
            p.starts[p.runIndex[i]] = i;
        }
        if (i == 0u) {
            p.starts[liveCount(p.runTotal, 0u, p.capacity)] = entries;
        }
    }
};

struct CellsParams {
    const u32* starts = nullptr;
    const u32* runTotal = nullptr;
    u32 capacity = 0;
    u32* bodies = nullptr;
    u32* uniqueCounts = nullptr;
    u32* pairCounts = nullptr;
};

/// broadphase_kernel::CellKernel for live runs; zero counts past them (grid = capacity).
struct CellsKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CellsParams& p) const {
        if (idx.linear >= liveCount(p.runTotal, 0u, p.capacity)) {
            p.uniqueCounts[idx.linear] = 0u;
            p.pairCounts[idx.linear] = 0u;
            return;
        }
        broadphase_kernel::CellKernel{}(idx, broadphase_kernel::CellParams{p.starts, p.bodies, p.uniqueCounts,
                                                                             p.pairCounts});
    }
};

struct PairsParams {
    const u32* starts = nullptr;
    const u32* runTotal = nullptr;
    u32 capacity = 0; ///< run capacity
    const u32* bodies = nullptr;
    const u32* uniqueCounts = nullptr;
    const u32* pairOffsets = nullptr;
    u32 bits = 16;
    u32* pairKeys = nullptr;
    u32 pairCapacity = 0;
    u32* clipped = nullptr;
};

/// broadphase_kernel::PairKernel with a capacity guard (grid = run capacity).
struct PairsKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PairsParams& p) const {
        if (idx.linear >= liveCount(p.runTotal, 0u, p.capacity)) {
            return;
        }
        const u32 u = p.uniqueCounts[idx.linear];
        if (u < 2u) {
            return;
        }
        const u32* v = p.bodies + p.starts[idx.linear];
        u32 write = p.pairOffsets[idx.linear];
        u32 dropped = 0u;
        for (u32 i = 0; i < u; ++i) {
            for (u32 j = i + 1u; j < u; ++j) {
                if (write < p.pairCapacity) {
                    p.pairKeys[write] = (v[i] << p.bits) | v[j];
                } else {
                    ++dropped;
                }
                ++write;
            }
        }
        if (dropped != 0u) {
            kernel::global_atomic_add(p.clipped, dropped);
        }
    }
};

/// collisionLayersCollide (physics_data.hpp), device-safe.
FUSE_HOST_DEVICE inline bool layersCollide(u32 layerA, u32 maskA, u32 layerB, u32 maskB) {
    return (layerA & maskB) != 0u && (layerB & maskA) != 0u;
}

struct PlanePairsParams {
    const u32* shapeTypes = nullptr;
    const u32* shapeBodies = nullptr;
    u32 shapeCount = 0;
    const u32* flags = nullptr;
    const u32* layers = nullptr; ///< may be null (every pair passes, like a short layer array)
    const u32* masks = nullptr;
    u32 layerCount = 0;
    u32 bodyCount = 0;
    const u32* planeBodies = nullptr;
    u32 planeCount = 0;
    const u32* cellPairTotal = nullptr; ///< plane slots follow the cell pairs
    u32 bits = 16;
    u32 sentinel = 0;
    u32* pairKeys = nullptr;
    u32 pairCapacity = 0;
    u32* clipped = nullptr;
};

/// One slot per (shape, plane body): the canonical pair key when the shape is a non-plane shape of a
/// non-static body and the pair is valid and passes the collision layers (the plane merge of
/// mergePlanePairsAndClamp), else the sentinel. The sort + unique that follows merges them with the
/// cell pairs (grid = shapes).
struct PlanePairsKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PlanePairsParams& p) const {
        const u32 s = idx.linear;
        const u32 body = p.shapeBodies[s];
        const bool dynamic = body < p.bodyCount &&
                             static_cast<CollisionShapeType>(p.shapeTypes[s]) != CollisionShapeType::Plane &&
                             (p.flags[body] & RB_STATIC) == 0u;
        const u32 base = liveCount(p.cellPairTotal, 0u, 0xFFFFFFFFu) + s * p.planeCount;
        u32 dropped = 0u;
        for (u32 j = 0; j < p.planeCount; ++j) {
            const u32 plane = p.planeBodies[j];
            u32 key = p.sentinel;
            if (dynamic && body != plane && plane < p.bodyCount) {
                const bool layersPass = p.layers == nullptr || body >= p.layerCount || plane >= p.layerCount ||
                                        layersCollide(p.layers[body], p.masks[body], p.layers[plane], p.masks[plane]);
                if (layersPass) {
                    const u32 lo = body < plane ? body : plane;
                    const u32 hi = body < plane ? plane : body;
                    key = (lo << p.bits) | hi;
                }
            }
            if (base + j < p.pairCapacity) {
                p.pairKeys[base + j] = key;
            } else {
                ++dropped;
            }
        }
        if (dropped != 0u) {
            kernel::global_atomic_add(p.clipped, dropped);
        }
    }
};

struct UniqueFlagParams {
    const u32* keys = nullptr;
    u32 sentinel = 0;
    u32* flags = nullptr;
};

/// flags[i] = 1 for the first of each run of equal live keys (grid = pair capacity).
struct UniqueFlagKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const UniqueFlagParams& p) const {
        const u32 i = idx.linear;
        p.flags[i] = p.keys[i] != p.sentinel && broadphase_kernel::runBoundary(p.keys, i) ? 1u : 0u;
    }
};

struct UniqueWriteParams {
    const u32* keys = nullptr;
    const u32* index = nullptr; ///< scanned unique flags
    u32 sentinel = 0;
    u32* out = nullptr;
};

/// out[index[i]] = keys[i] for the first key of each run (grid = pair capacity).
struct UniqueWriteKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const UniqueWriteParams& p) const {
        const u32 i = idx.linear;
        if (p.keys[i] != p.sentinel && broadphase_kernel::runBoundary(p.keys, i)) {
            p.out[p.index[i]] = p.keys[i];
        }
    }
};

struct StatusParams {
    Status* status = nullptr;
    const u32* entryTotal = nullptr;
    u32 entryCapacity = 0;
    const u32* runTotal = nullptr;
    const u32* cellPairTotal = nullptr;
    u32 planeSlots = 0;
    u32 pairCapacity = 0;
    const u32* pairTotal = nullptr;
    const u32* contactTotal = nullptr;
    u32 contactCapacity = 0;
    const u32* clipped = nullptr;
};

/// Gathers the scan totals into the Status block and flags overflowed capacities (grid = 1). Null
/// totals leave their fields unchanged, so the broadphase and narrowphase can update it separately.
struct StatusKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex&, const StatusParams& p) const {
        Status& s = *p.status;
        if (p.entryTotal != nullptr) {
            s.entries = *p.entryTotal;
            s.cells = *p.runTotal;
            s.cellPairs = *p.cellPairTotal;
            s.pairSlots = s.cellPairs + p.planeSlots;
            s.pairs = *p.pairTotal;
            s.overflow = (s.entries > p.entryCapacity ? kOverflowEntries : 0u) |
                         (s.pairSlots > p.pairCapacity ? kOverflowPairs : 0u);
        }
        if (p.contactTotal != nullptr) {
            s.contacts = *p.contactTotal;
            s.overflow = (s.overflow & ~kOverflowContacts) | (s.contacts > p.contactCapacity ? kOverflowContacts : 0u);
        }
        if (p.clipped != nullptr) {
            s.clipped = *p.clipped;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Narrowphase: pair preflight + primitive dispatch + manifold finalize (ports of contact_pair.cpp /
// contact_manifold.cpp / friction.cpp for the default epsilons; fuse_b4_physics_kernel_gates checks
// they give the CPU path's manifolds bit for bit).
// ---------------------------------------------------------------------------------------------

/// Bodies as the narrowphase reads them.
struct NarrowBodies {
    const vec3* positions = nullptr;
    const quat* orientations = nullptr;
    u32 orientationCount = 0;
    const u32* flags = nullptr;
    const f32* invMasses = nullptr;
    u32 bodyCount = 0;
};

/// Shapes plus the shape the narrowphase uses for each body (findShapeForBody with the sphere
/// preference; shapeCount when the body has none), built on the host (ResidentPhysics::uploadShapes).
struct NarrowShapes {
    const u32* types = nullptr;
    const vec3* params = nullptr;
    const f32* scalars = nullptr;
    u32 shapeCount = 0;
    const u32* bodyShape = nullptr;
};

FUSE_HOST_DEVICE inline bool shapeDegenerate(CollisionShapeType type, const vec3& params) {
    switch (type) {
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
        return params.x <= 0.f;
    case CollisionShapeType::Box:
    case CollisionShapeType::ConvexHull:
        return params.x <= 0.f || params.y <= 0.f || params.z <= 0.f;
    case CollisionShapeType::Plane:
        return params.length() < 1e-8f;
    default:
        return false;
    }
}

/// True when the CPU narrowphase rejects the pair before dispatch: contact_pair_reject_reason, plus
/// (deepen) the contact_pair_deepen_reject_reason checks the pipeline narrowphase applies.
FUSE_HOST_DEVICE inline bool pairRejected(const NarrowBodies& b, const NarrowShapes& s, u32 a, u32 c, bool deepen) {
    if (a == c || a >= b.bodyCount || c >= b.bodyCount) {
        return true;
    }
    const u32 shapeA = s.bodyShape[a];
    const u32 shapeB = s.bodyShape[c];
    if (shapeA >= s.shapeCount || shapeB >= s.shapeCount) {
        return true; // MissingShape
    }
    const u32 flagsA = b.flags[a];
    const u32 flagsB = b.flags[c];
    if ((flagsA & RB_TRIGGER) != 0u && (flagsB & RB_TRIGGER) != 0u) {
        return true; // BothTriggers
    }
    const CollisionShapeType typeA = static_cast<CollisionShapeType>(s.types[shapeA]);
    const CollisionShapeType typeB = static_cast<CollisionShapeType>(s.types[shapeB]);
    // UnsupportedShapePair: the device dispatches primitives only (scenes with convex hulls are
    // refused at upload), and plane-plane has no dispatch.
    if (!np::isPrimitiveShape(typeA) || !np::isPrimitiveShape(typeB) ||
        (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane)) {
        return true;
    }
    if ((flagsA & RB_STATIC) != 0u && (flagsB & RB_STATIC) != 0u) {
        return true; // BothStatic
    }
    const vec3 paramsA = s.params[shapeA];
    const vec3 paramsB = s.params[shapeB];
    if (shapeDegenerate(typeA, paramsA) || shapeDegenerate(typeB, paramsB)) {
        return true;
    }
    if (!deepen) {
        return false;
    }
    if ((typeA == CollisionShapeType::Capsule && paramsA.y <= 0.f) ||
        (typeB == CollisionShapeType::Capsule && paramsB.y <= 0.f)) {
        return true; // deepen DegenerateShape
    }
    if ((flagsA & RB_SLEEPING) != 0u && (flagsB & RB_SLEEPING) != 0u) {
        return true;
    }
    if ((flagsA & RB_KINEMATIC) != 0u && (flagsB & RB_KINEMATIC) != 0u) {
        return true;
    }
    if ((flagsA & RB_TRIGGER) != 0u || (flagsB & RB_TRIGGER) != 0u) {
        return true;
    }
    return b.invMasses[a] <= 1e-8f && b.invMasses[c] <= 1e-8f; // BothMassless
}

/// dispatchShapePair: the primitive dispatch at the bodies' poses, minSeparation against the centres.
FUSE_HOST_DEVICE inline np::ContactManifold dispatchPair(const NarrowBodies& b, const NarrowShapes& s, u32 a, u32 c,
                                                         f32 margin) {
    const u32 shapeA = s.bodyShape[a];
    const u32 shapeB = s.bodyShape[c];
    np::ShapeInstance instA{};
    instA.type = static_cast<CollisionShapeType>(s.types[shapeA]);
    instA.params = s.params[shapeA];
    instA.scalar = s.scalars[shapeA];
    instA.position = b.positions[a];
    instA.orientation = a < b.orientationCount ? b.orientations[a] : quat{};
    np::ShapeInstance instB{};
    instB.type = static_cast<CollisionShapeType>(s.types[shapeB]);
    instB.params = s.params[shapeB];
    instB.scalar = s.scalars[shapeB];
    instB.position = b.positions[c];
    instB.orientation = c < b.orientationCount ? b.orientations[c] : quat{};
    np::ContactManifold manifold = np::collidePrimitiveShapes(instA, instB, a, c, margin);
    if (manifold.valid) {
        const vec3 centres = b.positions[manifold.bodyA] - b.positions[manifold.bodyB];
        manifold.minSeparation = centres.dot(manifold.contactNormal) + manifold.maxPenetration();
    }
    return manifold;
}

namespace finalize {

inline constexpr f32 kSeparationEpsilon = 1e-6f;
inline constexpr f32 kDuplicateEpsilon = 1e-4f;
inline constexpr f32 kFrictionEpsilon = 1e-4f;

FUSE_HOST_DEVICE inline bool validNormal(const np::ContactManifold& m) {
    return m.contactNormal.length() > 1e-6f;
}

FUSE_HOST_DEVICE inline bool hasSeparated(const np::ContactManifold& m) {
    for (u32 i = 0u; i < m.pointCount; ++i) {
        if (m.points[i].penetration < -kSeparationEpsilon) {
            return true;
        }
    }
    return false;
}

FUSE_HOST_DEVICE inline bool hasDuplicates(const np::ContactManifold& m) {
    if (m.pointCount <= 1u) {
        return false;
    }
    const f32 epsilonSq = kDuplicateEpsilon * kDuplicateEpsilon;
    for (u32 i = 0u; i < m.pointCount; ++i) {
        for (u32 j = i + 1u; j < m.pointCount; ++j) {
            const vec3 delta = m.points[i].point - m.points[j].point;
            if (delta.dot(delta) <= epsilonSq) {
                return true;
            }
        }
    }
    return false;
}

/// ContactManifold::wouldBeEmptyAfterPrune.
FUSE_HOST_DEVICE inline bool wouldBeEmptyAfterPrune(const np::ContactManifold& m) {
    if (m.pointCount == 0u) {
        return true;
    }
    np::ContactPoint surviving[np::kMaxContactPointsPerManifold]{};
    u32 survivingCount = 0u;
    const f32 epsilonSq = kDuplicateEpsilon * kDuplicateEpsilon;
    for (u32 readIndex = 0u; readIndex < m.pointCount; ++readIndex) {
        if (m.points[readIndex].penetration < -kSeparationEpsilon) {
            continue;
        }
        bool duplicate = false;
        for (u32 existing = 0u; existing < survivingCount; ++existing) {
            const vec3 delta = m.points[readIndex].point - surviving[existing].point;
            if (delta.dot(delta) <= epsilonSq) {
                if (m.points[readIndex].penetration > surviving[existing].penetration) {
                    surviving[existing] = m.points[readIndex];
                }
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (survivingCount < np::kMaxContactPointsPerManifold) {
            surviving[survivingCount] = m.points[readIndex];
            ++survivingCount;
        }
    }
    return survivingCount == 0u;
}

/// ContactManifold::needsPruning.
FUSE_HOST_DEVICE inline bool needsPruning(const np::ContactManifold& m) {
    if (m.pointCount == 0u) {
        return false;
    }
    u32 penetrating = 0u;
    for (u32 i = 0u; i < m.pointCount; ++i) {
        if (m.points[i].penetration < -kSeparationEpsilon) {
            return true;
        }
        if (m.points[i].penetration >= -kSeparationEpsilon) {
            ++penetrating;
        }
    }
    if (penetrating > np::kMaxContactPointsPerManifold) {
        return true;
    }
    return hasDuplicates(m);
}

/// ContactManifold::pruneContactPoints: separated points, duplicates (the deeper one wins), at most
/// kMaxContactPointsPerManifold points (always true already: addPoint caps the count).
FUSE_HOST_DEVICE inline void pruneContactPoints(np::ContactManifold& m) {
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < m.pointCount; ++readIndex) {
        if (m.points[readIndex].penetration < -kSeparationEpsilon) {
            continue;
        }
        if (writeIndex != readIndex) {
            m.points[writeIndex] = m.points[readIndex];
        }
        ++writeIndex;
    }
    for (u32 i = writeIndex; i < m.pointCount; ++i) {
        m.points[i] = {};
    }
    m.pointCount = writeIndex;
    m.syncLegacyFields();

    if (m.pointCount > 1u) {
        const f32 epsilonSq = kDuplicateEpsilon * kDuplicateEpsilon;
        writeIndex = 0u;
        for (u32 readIndex = 0u; readIndex < m.pointCount; ++readIndex) {
            bool duplicate = false;
            for (u32 existing = 0u; existing < writeIndex; ++existing) {
                const vec3 delta = m.points[readIndex].point - m.points[existing].point;
                if (delta.dot(delta) <= epsilonSq) {
                    if (m.points[readIndex].penetration > m.points[existing].penetration) {
                        m.points[existing] = m.points[readIndex];
                    }
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (writeIndex != readIndex) {
                m.points[writeIndex] = m.points[readIndex];
            }
            ++writeIndex;
        }
        for (u32 i = writeIndex; i < m.pointCount; ++i) {
            m.points[i] = {};
        }
        m.pointCount = writeIndex;
        m.syncLegacyFields();
    }
}

/// buildTangentBasis (friction.cpp).
FUSE_HOST_DEVICE inline np::TangentBasis tangentBasis(vec3 normal) {
    const vec3 unitNormal = normal.normalized();
    const vec3 reference = std::fabs(unitNormal.y) < 0.99f ? vec3{0.f, 1.f, 0.f} : vec3{1.f, 0.f, 0.f};
    const vec3 tangent1 = reference.cross(unitNormal).normalized();
    const vec3 tangent2 = unitNormal.cross(tangent1).normalized();
    return {tangent1, tangent2};
}

/// isOrthonormalTangentBasis (friction.cpp).
FUSE_HOST_DEVICE inline bool orthonormal(vec3 normal, const np::TangentBasis& basis, f32 epsilon) {
    const vec3 unitNormal = normal.normalized();
    const f32 tangent1Length = basis.tangent1.length();
    const f32 tangent2Length = basis.tangent2.length();
    if (std::fabs(tangent1Length - 1.f) > epsilon || std::fabs(tangent2Length - 1.f) > epsilon) {
        return false;
    }
    return std::fabs(basis.tangent1.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent2.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent1.dot(basis.tangent2)) <= epsilon;
}

/// ContactManifold::hasFrictionBasis.
FUSE_HOST_DEVICE inline bool hasFrictionBasis(const np::ContactManifold& m) {
    return validNormal(m) && orthonormal(m.contactNormal, m.frictionBasis, 1e-4f);
}

/// friction_basis_is_stale.
FUSE_HOST_DEVICE inline bool basisStale(const np::ContactManifold& m) {
    if (m.pointCount == 0u || !validNormal(m)) {
        return false;
    }
    const bool partial = m.frictionBasis.tangent1.length() > kFrictionEpsilon ||
                         m.frictionBasis.tangent2.length() > kFrictionEpsilon;
    if (!partial) {
        return false;
    }
    return !(validNormal(m) && orthonormal(m.contactNormal, m.frictionBasis, kFrictionEpsilon));
}

/// compute_friction_tangents -> rebuild_friction_basis_with_preflight (non-empty, valid normal).
FUSE_HOST_DEVICE inline void frictionTangents(np::ContactManifold& m) {
    if (m.pointCount == 0u || !validNormal(m)) {
        m.frictionBasis = {};
        return;
    }
    const bool canReuse = hasFrictionBasis(m) && !basisStale(m);
    if (canReuse) {
        return;
    }
    const f32 length = m.contactNormal.length();
    if (validNormal(m) && std::fabs(length - 1.f) > kFrictionEpsilon && length > 1e-8f) {
        m.contactNormal = m.contactNormal * (1.f / length);
    }
    // rebuild_friction_basis_if_needed: reusable after the normalisation -> keep; else invalidate and
    // build (ensure_friction_basis).
    if (hasFrictionBasis(m) && !basisStale(m)) {
        return;
    }
    m.frictionBasis = {};
    if (m.contactNormal.length() < 1e-8f) {
        m.frictionBasis = {};
    } else {
        m.frictionBasis = tangentBasis(m.contactNormal);
    }
}

/// finalize_contact_manifold_with_preflight with the default epsilons. Returns false when the CPU
/// path returns false (the manifold is then not stored, whatever state it is left in).
FUSE_HOST_DEVICE inline bool finalizeManifold(np::ContactManifold& m) {
    if (m.pointCount == 0u || !validNormal(m) || wouldBeEmptyAfterPrune(m)) {
        return false;
    }
    u32 penetrating = 0u;
    for (u32 i = 0u; i < m.pointCount; ++i) {
        penetrating += m.points[i].penetration >= -kSeparationEpsilon ? 1u : 0u;
    }
    if (penetrating == 0u) {
        return false; // NoPenetratingPoints (only reachable with NaN depths)
    }
    if (hasSeparated(m) || hasDuplicates(m) || m.pointCount > np::kMaxContactPointsPerManifold) {
        if (needsPruning(m)) {
            pruneContactPoints(m);
        }
        if (m.pointCount == 0u) {
            return false;
        }
    }
    // generate_contact_manifold
    pruneContactPoints(m);
    if (m.pointCount == 0u || !validNormal(m)) {
        return false;
    }
    const f32 normalLength = m.contactNormal.length();
    m.contactNormal = m.contactNormal * (1.f / normalLength);
    m.syncLegacyFields();
    frictionTangents(m);
    if (!hasFrictionBasis(m)) {
        return false;
    }
    m.valid = true;
    return true;
}

} // namespace finalize

/// One candidate pair through the CPU narrowphase: narrowphasePairContact (speculative == false,
/// deepen preflight, no margin) or one collidePairs element (speculative == true: base preflight, a
/// `margin`-wide speculative band, penetration shifted around the finalize). False = no manifold.
FUSE_HOST_DEVICE inline bool detectPair(const NarrowBodies& b, const NarrowShapes& s, u32 a, u32 c, f32 margin,
                                        bool speculative, np::ContactManifold& out) {
    if (!speculative) {
        if (pairRejected(b, s, a, c, true)) {
            return false;
        }
        out = dispatchPair(b, s, a, c, 0.f);
        return finalize::finalizeManifold(out);
    }
    out = pairRejected(b, s, a, c, false) ? np::invalidContactManifold() : dispatchPair(b, s, a, c, margin);
    if (margin > 0.f) {
        for (u32 i = 0; i < out.pointCount; ++i) {
            out.points[i].penetration += margin;
        }
    }
    if (!finalize::finalizeManifold(out)) {
        return false;
    }
    if (margin > 0.f) {
        for (u32 i = 0; i < out.pointCount; ++i) {
            out.points[i].penetration -= margin;
        }
        out.syncLegacyFields();
    }
    return true;
}

struct DetectParams {
    NarrowBodies bodies{};
    NarrowShapes shapes{};
    const u32* pairKeys = nullptr; ///< (bodyA << bits) | bodyB, in narrowphase order
    const u32* pairTotal = nullptr; ///< live pair count (device); null: `pairCount`
    u32 pairCount = 0;
    u32 capacity = 0;
    u32 bits = 16;
    f32 margin = 0.f;
    u32 speculative = 0;
    np::ContactManifold* staging = nullptr; ///< slot per pair
    u32* flags = nullptr;                   ///< 1 when the pair produced a manifold
    u32* offsets = nullptr;                 ///< the same flag, scanned in place into contact slots
};

/// physics_resident_np_detect: one pair into its own staging slot (grid = pair capacity). On sm_86 the
/// inlined oriented box-box clip needs a ~1.4 KiB stack frame; the CUDA driver raises the per-thread
/// stack limit on the first launch (cudaLimitStackSize grows as needed), which happens in the warm-up.
struct DetectKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const DetectParams& p) const {
        const u32 i = idx.linear;
        const u32 live = p.pairTotal != nullptr ? liveCount(p.pairTotal, 0u, p.capacity)
                                                : (p.pairCount < p.capacity ? p.pairCount : p.capacity);
        u32 flag = 0u;
        if (i < live) {
            const u32 key = p.pairKeys[i];
            const u32 a = key >> p.bits;
            const u32 c = key & ((1u << p.bits) - 1u);
            np::ContactManifold manifold{};
            if (detectPair(p.bodies, p.shapes, a, c, p.margin, p.speculative != 0u, manifold)) {
                p.staging[i] = manifold;
                flag = 1u;
            }
        }
        p.flags[i] = flag;
        p.offsets[i] = flag;
    }
};

struct CompactParams {
    const np::ContactManifold* staging = nullptr;
    const u32* flags = nullptr;   ///< unscanned flags
    const u32* offsets = nullptr; ///< exclusive scan of the flags
    np::ContactManifold* out = nullptr;
    u32 outCapacity = 0;
};

/// physics_resident_np_compact: stable compaction of the produced manifolds (grid = pair capacity).
struct CompactKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CompactParams& p) const {
        const u32 i = idx.linear;
        if (p.flags[i] != 0u && p.offsets[i] < p.outCapacity) {
            p.out[p.offsets[i]] = p.staging[i];
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Coloured constraint solve (port of SolveColorKernel / solveContactConstraint /
// solveDistanceConstraint over plain arrays; same operation order, so the same floats).
// ---------------------------------------------------------------------------------------------

inline constexpr u32 kDistanceBit = 0x80000000u; ///< ConstraintColoring::kDistanceBit

struct SolverBodies {
    vec3* predictedPositions = nullptr;
    quat* predictedOrientations = nullptr;
    const u32* flags = nullptr;
    const f32* invMasses = nullptr;
    const f32* frictionStatic = nullptr;
    const vec3* invInertia = nullptr; ///< SolverWorkBuffers::bodyInvInertia
    u32 invInertiaCount = 0;
};

struct SolverConstraints {
    const np::ContactManifold* contacts = nullptr;
    const ContactAnchor* anchors = nullptr; ///< kMaxContactPointsPerManifold per contact
    u32 anchorCount = 0;
    f32* contactLambdas = nullptr;
    f32* pointLambdas = nullptr;
    u32 pointLambdaCount = 0;
    const DistanceConstraint* distances = nullptr;
    f32* distanceLambdas = nullptr;
};

struct BodyRef {
    u32 index = 0;
    f32 invMass = 0.f;
    vec3 invInertia{};
};

FUSE_HOST_DEVICE inline BodyRef solverBody(const SolverBodies& b, u32 index) {
    const u32 flags = b.flags[index];
    const bool pinned = (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u || (flags & RB_SLEEPING) != 0u;
    BodyRef ref{};
    ref.index = index;
    ref.invMass = pinned ? 0.f : b.invMasses[index];
    ref.invInertia = ref.invMass > 0.f && index < b.invInertiaCount ? b.invInertia[index] : vec3{};
    return ref;
}

FUSE_HOST_DEVICE inline vec3 anchorArm(const SolverBodies& b, u32 body, const vec3& localAnchor) {
    if (localAnchor.x == 0.f && localAnchor.y == 0.f && localAnchor.z == 0.f) {
        return {};
    }
    return rotate(b.predictedOrientations[body], localAnchor);
}

FUSE_HOST_DEVICE inline void applyPositionalImpulse(const SolverBodies& b, const BodyRef& A, const BodyRef& B, vec3 rA,
                                                    vec3 rB, vec3 impulse) {
    if (A.invMass > 0.f) {
        b.predictedPositions[A.index] += impulse * A.invMass;
        const vec3 dTheta = applyInverseInertia(b.predictedOrientations[A.index], A.invInertia, rA.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            b.predictedOrientations[A.index] = integrateRotation(b.predictedOrientations[A.index], dTheta);
        }
    }
    if (B.invMass > 0.f) {
        b.predictedPositions[B.index] -= impulse * B.invMass;
        const vec3 dTheta = applyInverseInertia(b.predictedOrientations[B.index], B.invInertia, rB.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            b.predictedOrientations[B.index] = integrateRotation(b.predictedOrientations[B.index], dTheta * -1.f);
        }
    }
}

/// solveDistanceConstraint.
FUSE_HOST_DEVICE inline void solveDistance(const SolverBodies& b, const DistanceConstraint& constraint,
                                           const BodyRef& A, const BodyRef& B, f32 dt, f32& lambda) {
    if (A.invMass + B.invMass < 1e-10f) {
        return;
    }
    const vec3 rA = anchorArm(b, A.index, constraint.localAnchorA);
    const vec3 rB = anchorArm(b, B.index, constraint.localAnchorB);
    const vec3 diff = (b.predictedPositions[A.index] + rA) - (b.predictedPositions[B.index] + rB);
    const f32 distance = diff.length();
    if (distance < 1e-8f) {
        return;
    }
    const vec3 n = diff * (1.f / distance);
    const f32 constraintValue = distance - constraint.restLength;
    const f32 w = generalizedInverseMass(A.invMass, b.predictedOrientations[A.index], A.invInertia, rA, n) +
                  generalizedInverseMass(B.invMass, b.predictedOrientations[B.index], B.invInertia, rB, n);
    const f32 alpha = constraint.compliance / (dt * dt);
    if (w + alpha < 1e-10f) {
        return;
    }
    const f32 deltaLambda = -constraintValue / (w + alpha);
    lambda += deltaLambda;
    applyPositionalImpulse(b, A, B, rA, rB, n * deltaLambda);
}

struct PointPair {
    vec3 pA{};
    vec3 pB{};
};

FUSE_HOST_DEVICE inline bool hasAnchors(const SolverConstraints& c, u32 contactIndex) {
    return (contactIndex + 1u) * np::kMaxContactPointsPerManifold <= c.anchorCount;
}

FUSE_HOST_DEVICE inline PointPair contactPoints(const SolverBodies& b, const SolverConstraints& c,
                                                const np::ContactManifold& contact, u32 contactIndex, u32 k) {
    const u32 a = contact.bodyA;
    const u32 bb = contact.bodyB;
    if (hasAnchors(c, contactIndex)) {
        const ContactAnchor& anchor = c.anchors[contactIndex * np::kMaxContactPointsPerManifold + k];
        return {b.predictedPositions[a] + rotate(b.predictedOrientations[a], anchor.localA),
                b.predictedPositions[bb] + rotate(b.predictedOrientations[bb], anchor.localB)};
    }
    return {b.predictedPositions[a], b.predictedPositions[bb] + contact.contactNormal * contact.minSeparation};
}

/// solveContactConstraint (block normal solve over the penetrating points + static friction).
FUSE_HOST_DEVICE inline void solveContact(const SolverBodies& b, const SolverConstraints& c, u32 contactIndex,
                                          const np::ContactManifold& contact, const BodyRef& A, const BodyRef& B,
                                          f32 dt, f32 contactCompliance, f32& lambda) {
    if (!contact.valid || A.invMass + B.invMass < 1e-10f) {
        return;
    }
    constexpr u32 kSlots = np::kMaxContactPointsPerManifold;
    const vec3 n = contact.contactNormal;
    const f32 alpha = contactCompliance / (dt * dt);
    const quat qA = b.predictedOrientations[A.index];
    const quat qB = b.predictedOrientations[B.index];

    u32 active[kSlots]{};
    f32 target[kSlots]{};
    f32 step[kSlots]{};
    vec3 armA[kSlots]{};
    vec3 armB[kSlots]{};
    vec3 spinA[kSlots]{};
    vec3 spinB[kSlots]{};
    u32 count = 0;
    for (u32 k = 0; k < contact.pointCount && k < kSlots; ++k) {
        const PointPair points = contactPoints(b, c, contact, contactIndex, k);
        const f32 separation = (points.pA - points.pB).dot(n);
        if (separation >= 0.f) {
            continue;
        }
        const vec3 rA = points.pA - b.predictedPositions[A.index];
        const vec3 rB = points.pB - b.predictedPositions[B.index];
        spinA[count] = applyInverseInertia(qA, A.invInertia, rA.cross(n));
        spinB[count] = applyInverseInertia(qB, B.invInertia, rB.cross(n));
        const f32 w = A.invMass + B.invMass + rA.cross(n).dot(spinA[count]) + rB.cross(n).dot(spinB[count]);
        active[count] = k;
        target[count] = -separation;
        step[count] = -separation / (w + alpha);
        armA[count] = rA;
        armB[count] = rB;
        ++count;
    }
    if (count == 0u) {
        return;
    }
    f32 scale = 1.f;
    if (count > 1u) {
        f32 targetDotMoved = 0.f;
        f32 movedSq = 0.f;
        for (u32 i = 0; i < count; ++i) {
            const vec3 ci = armA[i].cross(n);
            const vec3 di = armB[i].cross(n);
            f32 moved = 0.f;
            for (u32 j = 0; j < count; ++j) {
                moved += step[j] * (A.invMass + B.invMass + ci.dot(spinA[j]) + di.dot(spinB[j]));
            }
            targetDotMoved += target[i] * moved;
            movedSq += moved * moved;
        }
        scale = movedSq > 1e-20f ? std::clamp(targetDotMoved / movedSq, 0.f, static_cast<f32>(count)) : 0.f;
    }
    f32 blockLambda = 0.f;
    vec3 thetaA{};
    vec3 thetaB{};
    for (u32 i = 0; i < count; ++i) {
        const f32 deltaLambda = step[i] * scale;
        blockLambda += deltaLambda;
        const u32 slot = contactIndex * kSlots + active[i];
        if (slot < c.pointLambdaCount) {
            c.pointLambdas[slot] += deltaLambda;
        }
        thetaA += spinA[i] * deltaLambda;
        thetaB += spinB[i] * deltaLambda;
    }
    lambda += blockLambda;
    if (A.invMass > 0.f) {
        b.predictedPositions[A.index] += n * (blockLambda * A.invMass);
        if (thetaA.dot(thetaA) > 0.f) {
            b.predictedOrientations[A.index] = integrateRotation(qA, thetaA);
        }
    }
    if (B.invMass > 0.f) {
        b.predictedPositions[B.index] -= n * (blockLambda * B.invMass);
        if (thetaB.dot(thetaB) > 0.f) {
            b.predictedOrientations[B.index] = integrateRotation(qB, thetaB * -1.f);
        }
    }

    const f32 staticCoeff = std::sqrt(b.frictionStatic[A.index] * b.frictionStatic[B.index]);
    if (!hasAnchors(c, contactIndex) || staticCoeff <= 0.f) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 k = active[i];
        const PointPair points = contactPoints(b, c, contact, contactIndex, k);
        const ContactAnchor& anchor = c.anchors[contactIndex * kSlots + k];
        const vec3 displacement = (points.pA - points.pB) - anchor.startSeparation;
        const vec3 tangential = displacement - n * displacement.dot(n);
        const f32 tangentialLength = tangential.length();
        if (tangentialLength <= 1e-9f) {
            continue;
        }
        const vec3 t = tangential * (1.f / tangentialLength);
        const vec3 rA = points.pA - b.predictedPositions[A.index];
        const vec3 rB = points.pB - b.predictedPositions[B.index];
        const f32 wTangent =
            generalizedInverseMass(A.invMass, b.predictedOrientations[A.index], A.invInertia, rA, t) +
            generalizedInverseMass(B.invMass, b.predictedOrientations[B.index], B.invInertia, rB, t);
        const u32 slot = contactIndex * kSlots + k;
        const f32 pointLambda = slot < c.pointLambdaCount ? c.pointLambdas[slot] : 0.f;
        if (wTangent < 1e-10f || tangentialLength > staticCoeff * pointLambda * wTangent) {
            continue;
        }
        applyPositionalImpulse(b, A, B, rA, rB, t * (-tangentialLength / wTangent));
    }
}

/// One constraint reference of ConstraintColoring::items (kDistanceBit set: distance constraint).
FUSE_HOST_DEVICE inline void solveConstraintRef(const SolverBodies& b, const SolverConstraints& c, u32 ref, f32 dt,
                                                f32 compliance) {
    if ((ref & kDistanceBit) != 0u) {
        const u32 index = ref & ~kDistanceBit;
        const DistanceConstraint& constraint = c.distances[index];
        solveDistance(b, constraint, solverBody(b, constraint.bodyA), solverBody(b, constraint.bodyB), dt,
                      c.distanceLambdas[index]);
        return;
    }
    const np::ContactManifold& contact = c.contacts[ref];
    solveContact(b, c, ref, contact, solverBody(b, contact.bodyA), solverBody(b, contact.bodyB), dt, compliance,
                 c.contactLambdas[ref]);
}

struct SolveParams {
    SolverBodies bodies{};
    SolverConstraints constraints{};
    const u32* items = nullptr; ///< this colour's constraint refs
    u32 count = 0;              ///< SolveSerialKernel: refs to solve in order
    f32 dt = 0.f;
    f32 compliance = 0.f;
};

/// physics_resident_solve_color: the constraints of one colour share no dynamic body (grid = colour size).
struct SolveColorKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SolveParams& p) const {
        solveConstraintRef(p.bodies, p.constraints, p.items[idx.linear], p.dt, p.compliance);
    }
};

/// physics_resident_solve_serial: the uncoloured overflow list in build order (grid = 1).
struct SolveSerialKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex&, const SolveParams& p) const {
        for (u32 i = 0; i < p.count; ++i) {
            solveConstraintRef(p.bodies, p.constraints, p.items[i], p.dt, p.compliance);
        }
    }
};

} // namespace fuse::physics::resident
