#include <fuse/physics/broadphase/spatial_hash.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fuse::physics::broadphase {

namespace {

constexpr u32 kBuildGrainSize = 8u;
constexpr u32 kCellGrainSize = 4u;
constexpr u32 kPlanePairGrainSize = 16u;

f32 shapeRadius(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return 0.5f;
    }
    return shapes.params[shapeIndex].x;
}

u32 shapeBodyIndex(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return 0;
    }
    return shapes.bodyIndices[shapeIndex];
}

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

CandidatePair canonicalPair(u32 bodyA, u32 bodyB) {
    if (bodyA > bodyB) {
        std::swap(bodyA, bodyB);
    }
    return {bodyA, bodyB};
}

void appendPair(std::vector<CandidatePair>& pairs, u32 bodyA, u32 bodyB) {
    if (bodyA == bodyB) {
        return;
    }
    pairs.push_back(canonicalPair(bodyA, bodyB));
}

void dedupePairs(std::vector<CandidatePair>& pairs) {
    std::sort(pairs.begin(), pairs.end(), [](const CandidatePair& left, const CandidatePair& right) {
        return left.bodyA < right.bodyA || (left.bodyA == right.bodyA && left.bodyB < right.bodyB);
    });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
                            [](const CandidatePair& left, const CandidatePair& right) {
                                return left.bodyA == right.bodyA && left.bodyB == right.bodyB;
                            }),
                pairs.end());
}

struct CellBuckets {
    explicit CellBuckets(u32 tableSize) : buckets(tableSize), locks(tableSize) {}

    std::vector<std::vector<u32>> buckets;
    std::vector<std::mutex> locks;

    void insert(u32 key, u32 bodyIndex) {
        std::lock_guard<std::mutex> guard(locks[key]);
        buckets[key].push_back(bodyIndex);
    }
};

void generatePairsForCell(const std::vector<u32>& occupants, std::vector<CandidatePair>& out) {
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());

    for (usize i = 0; i < uniqueBodies.size(); ++i) {
        for (usize j = i + 1; j < uniqueBodies.size(); ++j) {
            appendPair(out, uniqueBodies[i], uniqueBodies[j]);
        }
    }
}

void populateShapeCells(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    CellBuckets& cells) {
    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    if (bodyIndex >= bodies.count()) {
        return;
    }

    const vec3 position = bodies.positions[bodyIndex];
    const f32 radius = shapeRadius(shapes, shapeIndex);
    const f32 cellSize = params.cellSize > 0.f ? params.cellSize : 1.f;
    const u32 tableSize = params.tableSize > 0 ? params.tableSize : 1u;

    if (use2D) {
        const ivec2 minCell = worldToCell2D({position.x, position.y}, cellSize);
        const ivec2 maxCell = worldToCell2D({position.x + radius, position.y + radius}, cellSize);
        for (s32 cy = minCell.y; cy <= maxCell.y; ++cy) {
            for (s32 cx = minCell.x; cx <= maxCell.x; ++cx) {
                const u32 key = spatialHash2D(cx, cy, tableSize);
                cells.insert(key, bodyIndex);
            }
        }
        return;
    }

    const ivec3 minCell =
        worldToCell({position.x - radius, position.y - radius, position.z - radius}, cellSize);
    const ivec3 maxCell =
        worldToCell({position.x + radius, position.y + radius, position.z + radius}, cellSize);
    for (s32 cz = minCell.z; cz <= maxCell.z; ++cz) {
        for (s32 cy = minCell.y; cy <= maxCell.y; ++cy) {
            for (s32 cx = minCell.x; cx <= maxCell.x; ++cx) {
                const u32 key = spatialHash(cx, cy, cz, tableSize);
                cells.insert(key, bodyIndex);
            }
        }
    }
}

std::vector<CandidatePair> runBroadphaseInternal(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    std::vector<CandidatePair> pairs;
    if (bodies.count() == 0 || shapes.count() == 0) {
        return pairs;
    }

    const u32 tableSize = params.tableSize > 0 ? params.tableSize : 1024u;
    CellBuckets cells(tableSize);

    const u32 shapeCount = shapes.count();
    fuse::jobs::parallel_for(0u, shapeCount, kBuildGrainSize, [&](u32 shapeIndex) {
        populateShapeCells(shapeIndex, bodies, shapes, params, use2D, cells);
    });

    std::vector<std::vector<CandidatePair>> cellPairs(tableSize);
    fuse::jobs::parallel_for(0u, tableSize, kCellGrainSize, [&](u32 cellIndex) {
        if (cells.buckets[cellIndex].empty()) {
            return;
        }
        generatePairsForCell(cells.buckets[cellIndex], cellPairs[cellIndex]);
    });

    for (const std::vector<CandidatePair>& bucketPairs : cellPairs) {
        pairs.insert(pairs.end(), bucketPairs.begin(), bucketPairs.end());
    }
    dedupePairs(pairs);

    std::vector<u32> planeBodies;
    std::vector<u32> dynamicBodies;
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (shapeType(shapes, shapeIndex) == CollisionShapeType::Plane) {
            planeBodies.push_back(bodyIndex);
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            dynamicBodies.push_back(bodyIndex);
        }
    }

    if (!planeBodies.empty() && !dynamicBodies.empty()) {
        std::unordered_set<u64> existing;
        existing.reserve(pairs.size() * 2 + 1);
        for (const CandidatePair& pair : pairs) {
            const u64 key = (static_cast<u64>(pair.bodyA) << 32) | pair.bodyB;
            existing.insert(key);
        }

        const u32 dynamicCount = static_cast<u32>(dynamicBodies.size());
        std::vector<std::vector<CandidatePair>> dynamicPlanePairs(dynamicCount);
        fuse::jobs::parallel_for(0u, dynamicCount, kPlanePairGrainSize, [&](u32 dynamicIndex) {
            const u32 dynamicBody = dynamicBodies[dynamicIndex];
            for (u32 planeBody : planeBodies) {
                if (planeBody == dynamicBody) {
                    continue;
                }
                const CandidatePair pair = canonicalPair(dynamicBody, planeBody);
                const u64 key = (static_cast<u64>(pair.bodyA) << 32) | pair.bodyB;
                if (existing.find(key) == existing.end()) {
                    dynamicPlanePairs[dynamicIndex].push_back(pair);
                }
            }
        });

        for (const std::vector<CandidatePair>& bucketPairs : dynamicPlanePairs) {
            pairs.insert(pairs.end(), bucketPairs.begin(), bucketPairs.end());
        }
        dedupePairs(pairs);
    }

    return pairs;
}

} // namespace

std::vector<CandidatePair> runBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params) {
    return runBroadphaseInternal(bodies, shapes, params, false);
}

std::vector<CandidatePair> runBroadphase2D(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params) {
    return runBroadphaseInternal(bodies, shapes, params, true);
}

} // namespace fuse::physics::broadphase
