#include <fuse/physics/ccd/ccd.hpp>

namespace fuse::physics {

bool CcdPipeline::shapeIsSphere(const CollisionShapeSoA& shapes, u32 shapeIndex, f32& outRadius) const {
    if (shapeIndex >= shapes.count() ||
        static_cast<CollisionShapeType>(shapes.types[shapeIndex]) != CollisionShapeType::Sphere) {
        return false;
    }
    outRadius = shapes.params[shapeIndex].x;
    return true;
}

u32 CcdPipeline::sweepPairs(const RigidBodySoA& bodies,
                            const CollisionShapeSoA& shapes,
                            const std::vector<broadphase::CandidatePair>& pairs,
                            f32 dt,
                            std::vector<TOIResult>& outResults) const {
    outResults.clear();
    lastResultCount_ = 0;

    if (dt <= 0.f) {
        return 0;
    }

    for (const broadphase::CandidatePair& pair : pairs) {
        const u32 bodyA = pair.bodyA;
        const u32 bodyB = pair.bodyB;
        if (bodyA >= bodies.count() || bodyB >= bodies.count()) {
            continue;
        }

        const bool ccdA = (bodies.flags[bodyA] & RB_CCD) != 0u;
        const bool ccdB = (bodies.flags[bodyB] & RB_CCD) != 0u;
        if (!ccdA && !ccdB) {
            continue;
        }

        u32 shapeA = bodies.count();
        u32 shapeB = bodies.count();
        for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
            if (shapes.bodyIndices[shapeIndex] == bodyA) {
                shapeA = shapeIndex;
            }
            if (shapes.bodyIndices[shapeIndex] == bodyB) {
                shapeB = shapeIndex;
            }
        }
        if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
            continue;
        }

        f32 radiusA = 0.f;
        f32 radiusB = 0.f;
        if (!shapeIsSphere(shapes, shapeA, radiusA) || !shapeIsSphere(shapes, shapeB, radiusB)) {
            continue;
        }

        const vec3 velocityA = bodies.linearVelocities[bodyA] * dt;
        const vec3 velocityB = bodies.linearVelocities[bodyB] * dt;
        TOIResult result = sweptSphereSphere(
            bodies.positions[bodyA], velocityA, radiusA, bodies.positions[bodyB], velocityB, radiusB);
        if (!result.valid) {
            continue;
        }

        result.bodyA = bodyA;
        result.bodyB = bodyB;
        outResults.push_back(result);
    }

    lastResultCount_ = static_cast<u32>(outResults.size());
    return lastResultCount_;
}

} // namespace fuse::physics
