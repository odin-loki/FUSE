#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>

namespace fuse::physics {

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

bool bodyNeedsCcd(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count() && (bodies.flags[bodyIndex] & RB_CCD) != 0u;
}

TOIResult dispatchCcdPair(const broadphase::CandidatePair& pair,
                          const RigidBodySoA& bodies,
                          const CollisionShapeSoA& shapes,
                          f32 dt) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count() || dt <= 0.f) {
        return {};
    }

    if (!bodyNeedsCcd(bodies, pair.bodyA) && !bodyNeedsCcd(bodies, pair.bodyB)) {
        return {};
    }

    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return {};
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];
    const vec3 velA = bodies.linearVelocities[pair.bodyA] * dt;
    const vec3 velB = bodies.linearVelocities[pair.bodyB] * dt;

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSphereSphere(
            posA, velA, shapes.params[shapeA].x, posB, velB, shapes.params[shapeB].x);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Plane) {
        TOIResult result = sweptSpherePlane(
            posA, velA - velB, shapes.params[shapeA].x, shapes.params[shapeB], shapes.scalars[shapeB]);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSpherePlane(
            posB, velB - velA, shapes.params[shapeB].x, shapes.params[shapeA], shapes.scalars[shapeA]);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Box) {
        const f32 halfThickness = shapes.params[shapeB].z;
        TOIResult result = sweptSphereSlabZ(
            posA, velA - velB, shapes.params[shapeA].x, posB.z, halfThickness);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Sphere) {
        const f32 halfThickness = shapes.params[shapeA].z;
        TOIResult result = sweptSphereSlabZ(
            posB, velB - velA, shapes.params[shapeB].x, posA.z, halfThickness);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    return {};
}

} // namespace

void runCcdIntoBuffer(const std::vector<broadphase::CandidatePair>& pairs,
                      const RigidBodySoA& bodies,
                      const CollisionShapeSoA& shapes,
                      f32 dt,
                      ToiBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes; the slot layout matches the future parallel_for path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        const TOIResult result = dispatchCcdPair(pairs[pairIndex], bodies, shapes, dt);
        if (result.valid) {
            buffer.writeSlot(pairIndex, result);
        }
    }

    buffer.compact();
}

u32 CcdPipeline::sweepPairs(const RigidBodySoA& bodies,
                            const CollisionShapeSoA& shapes,
                            const std::vector<broadphase::CandidatePair>& pairs,
                            f32 dt,
                            std::vector<TOIResult>& outResults) const {
    ToiBufferSoA buffer;
    buffer.reserve(static_cast<u32>(pairs.size()));
    runCcdIntoBuffer(pairs, bodies, shapes, dt, buffer);
    outResults = buffer.toVector();
    lastResultCount_ = buffer.activeCount;
    return lastResultCount_;
}

} // namespace fuse::physics
