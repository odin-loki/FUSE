#include <fuse/physics/narrowphase/collision_dispatch.hpp>

#include <cmath>

namespace fuse::physics::narrowphase {

namespace {

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

u32 findShapeForBody(const CollisionShapeSoA& shapes, u32 bodyIndex, CollisionShapeType preferred) {
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex &&
            static_cast<CollisionShapeType>(shapes.types[i]) == preferred) {
            return i;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return i;
        }
    }
    return shapes.count();
}

} // namespace

std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    std::vector<ContactManifold> manifolds;
    manifolds.reserve(pairs.size());

    for (const broadphase::CandidatePair& pair : pairs) {
        if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
            continue;
        }

        const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
        const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
        if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
            continue;
        }

        const CollisionShapeType typeA = shapeType(shapes, shapeA);
        const CollisionShapeType typeB = shapeType(shapes, shapeB);
        const vec3 posA = bodies.positions[pair.bodyA];
        const vec3 posB = bodies.positions[pair.bodyB];

        ContactManifold manifold{};
        if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Sphere) {
            manifold = collideSphereSphere(
                posA,
                shapes.params[shapeA].x,
                posB,
                shapes.params[shapeB].x,
                pair.bodyA,
                pair.bodyB);
        } else if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Plane) {
            manifold = collideSpherePlane(
                posA,
                shapes.params[shapeA].x,
                shapes.params[shapeB],
                shapes.scalars[shapeB],
                pair.bodyA,
                pair.bodyB);
        } else if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Sphere) {
            const vec3 normal = shapes.params[shapeA];
            manifold = collideSpherePlane(
                posB,
                shapes.params[shapeB].x,
                {-normal.x, -normal.y, -normal.z},
                -shapes.scalars[shapeA],
                pair.bodyB,
                pair.bodyA);
        }

        if (manifold.valid) {
            manifolds.push_back(manifold);
        }
    }

    return manifolds;
}

} // namespace fuse::physics::narrowphase
