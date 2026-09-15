#include <fuse/physics/solver/constraint_accumulation.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {
namespace {

void addPositionDelta(std::vector<PositionDelta>& positionDeltas, u32 bodyIndex, const vec3& delta) {
    if (bodyIndex >= positionDeltas.size()) {
        return;
    }
    positionDeltas[bodyIndex].delta += delta;
    ++positionDeltas[bodyIndex].writeCount;
}

vec3 worldAnchor(const RigidBodySoA& bodies, u32 bodyIndex, const vec3& localAnchor) {
    (void)localAnchor;
    return bodies.predictedPositions[bodyIndex];
}

} // namespace

f32 accumulateDistanceSpringCorrection(const RigidBodySoA& bodies,
                                       const DistanceConstraint& constraint,
                                       f32 invMassA,
                                       f32 invMassB,
                                       f32 dt,
                                       f32& lambda,
                                       std::vector<PositionDelta>& positionDeltas) {
    const u32 a = constraint.bodyA;
    const u32 b = constraint.bodyB;
    const f32 weightSum = invMassA + invMassB;
    if (weightSum < 1e-10f) {
        return 0.f;
    }

    const vec3 anchorA = worldAnchor(bodies, a, constraint.localAnchorA);
    const vec3 anchorB = worldAnchor(bodies, b, constraint.localAnchorB);
    const vec3 diff = anchorA - anchorB;
    const f32 distance = diff.length();
    if (distance < 1e-8f) {
        return 0.f;
    }

    const vec3 normal = diff * (1.f / distance);
    const f32 constraintValue = distance - constraint.restLength;
    const f32 alpha = constraint.compliance / (dt * dt);
    const f32 deltaLambda = -constraintValue / (weightSum + alpha);
    lambda += deltaLambda;
    const vec3 deltaPosition = normal * deltaLambda;

    addPositionDelta(positionDeltas, a, deltaPosition * invMassA);
    addPositionDelta(positionDeltas, b, deltaPosition * (-invMassB));

    return std::fabs(constraintValue);
}

f32 accumulateContactCorrection(const RigidBodySoA& bodies,
                                const narrowphase::ContactManifold& contact,
                                f32 invMassA,
                                f32 invMassB,
                                f32 dt,
                                f32 contactCompliance,
                                f32& lambda,
                                std::vector<PositionDelta>& positionDeltas) {
    if (!contact.valid) {
        return 0.f;
    }

    const u32 a = contact.bodyA;
    const u32 b = contact.bodyB;
    const f32 weightSum = invMassA + invMassB;
    if (weightSum < 1e-10f) {
        return 0.f;
    }

    const vec3 pa = bodies.predictedPositions[a];
    const vec3 pb = bodies.predictedPositions[b];
    const vec3 diff = pa - pb;
    const f32 constraint = diff.dot(contact.contactNormal) - contact.minSeparation;
    if (constraint >= 0.f) {
        return 0.f;
    }

    const f32 alpha = contactCompliance / (dt * dt);
    const f32 deltaLambda = -constraint / (weightSum + alpha);
    lambda += deltaLambda;
    const vec3 deltaPosition = contact.contactNormal * deltaLambda;

    addPositionDelta(positionDeltas, a, deltaPosition * invMassA);
    addPositionDelta(positionDeltas, b, deltaPosition * (-invMassB));

    const vec3 relativeVelocity = (pa - pb) * (1.f / dt);
    const f32 normalVelocity = relativeVelocity.dot(contact.contactNormal);
    vec3 tangentialVelocity = relativeVelocity - contact.contactNormal * normalVelocity;
    const f32 tangentialLength = tangentialVelocity.length();
    if (tangentialLength > 1e-6f) {
        const f32 frictionCoeff = bodies.frictionDynamic[a];
        const f32 frictionCorrection =
            std::min(frictionCoeff * std::fabs(deltaLambda), tangentialLength * weightSum) / weightSum;
        const vec3 tangentialDir = tangentialVelocity * (1.f / tangentialLength);
        addPositionDelta(positionDeltas, a, tangentialDir * (-frictionCorrection * invMassA));
        addPositionDelta(positionDeltas, b, tangentialDir * (frictionCorrection * invMassB));
    }

    return std::fabs(constraint);
}

f32 measureConstraintResidual(const RigidBodySoA& bodies,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 invMassFilter(const RigidBodySoA&, u32)) {
    f32 maxViolation = 0.f;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        const f32 invMassA = invMassFilter(bodies, contact.bodyA);
        const f32 invMassB = invMassFilter(bodies, contact.bodyB);
        if (invMassA + invMassB < 1e-10f) {
            continue;
        }

        const vec3 diff =
            bodies.predictedPositions[contact.bodyA] - bodies.predictedPositions[contact.bodyB];
        const f32 constraint = diff.dot(contact.contactNormal) - contact.minSeparation;
        if (constraint < 0.f) {
            maxViolation = std::max(maxViolation, std::fabs(constraint));
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        const f32 invMassA = invMassFilter(bodies, constraint.bodyA);
        const f32 invMassB = invMassFilter(bodies, constraint.bodyB);
        if (invMassA + invMassB < 1e-10f) {
            continue;
        }

        const vec3 anchorA = worldAnchor(bodies, constraint.bodyA, constraint.localAnchorA);
        const vec3 anchorB = worldAnchor(bodies, constraint.bodyB, constraint.localAnchorB);
        const f32 distance = (anchorA - anchorB).length();
        maxViolation = std::max(maxViolation, std::fabs(distance - constraint.restLength));
    }

    return maxViolation;
}

} // namespace fuse::physics
