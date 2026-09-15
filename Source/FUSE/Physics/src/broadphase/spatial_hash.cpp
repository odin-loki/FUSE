#include <fuse/physics/broadphase/spatial_hash.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace fuse::physics::broadphase {

namespace {

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

void appendPair(std::vector<CandidatePair>& pairs, u32 bodyA, u32 bodyB) {
    if (bodyA == bodyB) {
        return;
    }
    if (bodyA > bodyB) {
        std::swap(bodyA, bodyB);
    }
    pairs.push_back({bodyA, bodyB});
}

bool pairExists(const std::vector<CandidatePair>& pairs, u32 bodyA, u32 bodyB) {
    if (bodyA > bodyB) {
        std::swap(bodyA, bodyB);
    }
    for (const CandidatePair& pair : pairs) {
        if (pair.bodyA == bodyA && pair.bodyB == bodyB) {
            return true;
        }
    }
    return false;
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

    std::unordered_map<u32, std::vector<u32>> cells;
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
        if (bodyIndex >= bodies.count()) {
            continue;
        }

        const vec3 position = bodies.positions[bodyIndex];
        const f32 radius = shapeRadius(shapes, shapeIndex);
        const f32 cellSize = params.cellSize > 0.f ? params.cellSize : 1.f;

        if (use2D) {
            const ivec2 minCell = worldToCell2D({position.x, position.y}, cellSize);
            const ivec2 maxCell = worldToCell2D({position.x + radius, position.y + radius}, cellSize);
            for (s32 cy = minCell.y; cy <= maxCell.y; ++cy) {
                for (s32 cx = minCell.x; cx <= maxCell.x; ++cx) {
                    const u32 key = spatialHash2D(cx, cy, params.tableSize);
                    cells[key].push_back(bodyIndex);
                }
            }
        } else {
            const ivec3 minCell = worldToCell({position.x - radius, position.y - radius, position.z - radius}, cellSize);
            const ivec3 maxCell = worldToCell({position.x + radius, position.y + radius, position.z + radius}, cellSize);
            for (s32 cz = minCell.z; cz <= maxCell.z; ++cz) {
                for (s32 cy = minCell.y; cy <= maxCell.y; ++cy) {
                    for (s32 cx = minCell.x; cx <= maxCell.x; ++cx) {
                        const u32 key = spatialHash(cx, cy, cz, params.tableSize);
                        cells[key].push_back(bodyIndex);
                    }
                }
            }
        }
    }

    for (auto& entry : cells) {
        auto& occupants = entry.second;
        std::sort(occupants.begin(), occupants.end());
        occupants.erase(std::unique(occupants.begin(), occupants.end()), occupants.end());

        for (usize i = 0; i < occupants.size(); ++i) {
            for (usize j = i + 1; j < occupants.size(); ++j) {
                const u32 bodyA = occupants[i];
                const u32 bodyB = occupants[j];
                if (!pairExists(pairs, bodyA, bodyB)) {
                    appendPair(pairs, bodyA, bodyB);
                }
            }
        }
    }

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

    for (u32 planeBody : planeBodies) {
        for (u32 dynamicBody : dynamicBodies) {
            if (planeBody == dynamicBody) {
                continue;
            }
            if (!pairExists(pairs, dynamicBody, planeBody)) {
                appendPair(pairs, dynamicBody, planeBody);
            }
        }
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
