#include <fuse/physics/solver/constraint_accumulation.hpp>

#include <fuse/physics/rotation.hpp>

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

/// Anchor offset from the body centre in world space at the predicted orientation.
vec3 anchorArm(const RigidBodySoA& bodies, u32 bodyIndex, const vec3& localAnchor) {
    if (localAnchor.x == 0.f && localAnchor.y == 0.f && localAnchor.z == 0.f) {
        return {};
    }
    return rotate(bodies.predictedOrientations[bodyIndex], localAnchor);
}

vec3 worldAnchor(const RigidBodySoA& bodies, u32 bodyIndex, const vec3& localAnchor) {
    return bodies.predictedPositions[bodyIndex] + anchorArm(bodies, bodyIndex, localAnchor);
}

/// Applies the positional impulse `impulse` (+ on A, - on B) at arms rA / rB.
void applyPositionalImpulse(RigidBodySoA& bodies,
                            const ContactBody& A,
                            const ContactBody& B,
                            vec3 rA,
                            vec3 rB,
                            vec3 impulse) {
    if (A.invMass > 0.f) {
        bodies.predictedPositions[A.index] += impulse * A.invMass;
        const vec3 dTheta = applyInverseInertia(bodies.predictedOrientations[A.index], A.invInertia, rA.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            bodies.predictedOrientations[A.index] = integrateRotation(bodies.predictedOrientations[A.index], dTheta);
        }
    }
    if (B.invMass > 0.f) {
        bodies.predictedPositions[B.index] -= impulse * B.invMass;
        const vec3 dTheta = applyInverseInertia(bodies.predictedOrientations[B.index], B.invInertia, rB.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            bodies.predictedOrientations[B.index] =
                integrateRotation(bodies.predictedOrientations[B.index], dTheta * -1.f);
        }
    }
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

f32 solveDistanceConstraint(RigidBodySoA& bodies,
                            const DistanceConstraint& constraint,
                            const ContactBody& A,
                            const ContactBody& B,
                            f32 dt,
                            f32& lambda) {
    if (A.invMass + B.invMass < 1e-10f) {
        return 0.f;
    }
    const vec3 rA = anchorArm(bodies, A.index, constraint.localAnchorA);
    const vec3 rB = anchorArm(bodies, B.index, constraint.localAnchorB);
    const vec3 diff = (bodies.predictedPositions[A.index] + rA) - (bodies.predictedPositions[B.index] + rB);
    const f32 distance = diff.length();
    if (distance < 1e-8f) {
        return constraint.restLength; // coincident anchors: no defined direction to push along
    }
    const vec3 n = diff * (1.f / distance);
    const f32 constraintValue = distance - constraint.restLength;
    const f32 w = generalizedInverseMass(A.invMass, bodies.predictedOrientations[A.index], A.invInertia, rA, n) +
                  generalizedInverseMass(B.invMass, bodies.predictedOrientations[B.index], B.invInertia, rB, n);
    const f32 alpha = constraint.compliance / (dt * dt);
    if (w + alpha < 1e-10f) {
        return std::fabs(constraintValue);
    }
    const f32 deltaLambda = -constraintValue / (w + alpha);
    lambda += deltaLambda;
    applyPositionalImpulse(bodies, A, B, rA, rB, n * deltaLambda);
    return std::fabs(constraintValue);
}

namespace {

struct ContactPointPair {
    vec3 pA{};
    vec3 pB{};
};

/// World positions of contact point `k` on both bodies at their current predicted pose.
ContactPointPair contactPointPair(const RigidBodySoA& bodies,
                                  const SolverWorkBuffers& work,
                                  const narrowphase::ContactManifold& contact,
                                  u32 contactIndex,
                                  u32 k) {
    const u32 a = contact.bodyA;
    const u32 b = contact.bodyB;
    if (work.hasContactAnchors(contactIndex)) {
        const ContactAnchor& anchor = work.contactAnchor(contactIndex, k);
        return {bodies.predictedPositions[a] + rotate(bodies.predictedOrientations[a], anchor.localA),
                bodies.predictedPositions[b] + rotate(bodies.predictedOrientations[b], anchor.localB)};
    }
    // No prepared anchors (direct callers): the legacy centre-line form
    // (xA - xB) . n >= minSeparation for every point, which carries no torque.
    (void)k;
    return {bodies.predictedPositions[a],
            bodies.predictedPositions[b] + contact.contactNormal * contact.minSeparation};
}

} // namespace

f32 solveContactConstraint(RigidBodySoA& bodies,
                           SolverWorkBuffers& work,
                           u32 contactIndex,
                           const narrowphase::ContactManifold& contact,
                           const ContactBody& A,
                           const ContactBody& B,
                           f32 dt,
                           f32 contactCompliance,
                           f32& lambda) {
    if (!contact.valid || A.invMass + B.invMass < 1e-10f) {
        return 0.f;
    }
    constexpr u32 kSlots = narrowphase::kMaxContactPointsPerManifold;
    const vec3 n = contact.contactNormal;
    const f32 alpha = contactCompliance / (dt * dt);
    const quat qA = bodies.predictedOrientations[A.index];
    const quat qB = bodies.predictedOrientations[B.index];

    // Normal solve over the manifold's penetrating points as one block. Gauss-Seidel point by
    // point lets whichever point is solved first take the load (a flat face contact is rank
    // deficient), which tilts resting boxes and feeds jitter. Instead every point computes its
    // own XPBD step, and the combined step is scaled by the least-squares factor that best
    // meets all the point targets: exact for a symmetric face, and never an N-fold overshoot.
    u32 active[kSlots]{};
    f32 target[kSlots]{};
    f32 step[kSlots]{};
    vec3 armA[kSlots]{};
    vec3 armB[kSlots]{};
    vec3 spinA[kSlots]{}; // I_A^-1 (rA x n)
    vec3 spinB[kSlots]{};
    u32 count = 0;
    f32 worst = 0.f;
    for (u32 k = 0; k < contact.pointCount && k < kSlots; ++k) {
        const ContactPointPair points = contactPointPair(bodies, work, contact, contactIndex, k);
        const f32 separation = (points.pA - points.pB).dot(n);
        if (separation >= 0.f) {
            continue;
        }
        worst = std::max(worst, -separation);
        const vec3 rA = points.pA - bodies.predictedPositions[A.index];
        const vec3 rB = points.pB - bodies.predictedPositions[B.index];
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
        return 0.f;
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
        work.contactPointLambda(contactIndex, active[i]) += deltaLambda;
        thetaA += spinA[i] * deltaLambda;
        thetaB += spinB[i] * deltaLambda;
    }
    lambda += blockLambda;
    if (A.invMass > 0.f) {
        bodies.predictedPositions[A.index] += n * (blockLambda * A.invMass);
        if (thetaA.dot(thetaA) > 0.f) {
            bodies.predictedOrientations[A.index] = integrateRotation(qA, thetaA);
        }
    }
    if (B.invMass > 0.f) {
        bodies.predictedPositions[B.index] -= n * (blockLambda * B.invMass);
        if (thetaB.dot(thetaB) > 0.f) {
            bodies.predictedOrientations[B.index] = integrateRotation(qB, thetaB * -1.f);
        }
    }

    // Static friction: cancel each point's tangential drift since the substep start while it
    // stays inside the friction cone (|dp_t| / w_t <= mu_s * lambda_n). Dynamic friction is
    // applied in the velocity pass.
    const f32 staticCoeff = std::sqrt(bodies.frictionStatic[A.index] * bodies.frictionStatic[B.index]);
    if (!work.hasContactAnchors(contactIndex) || staticCoeff <= 0.f) {
        return worst;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 k = active[i];
        const ContactPointPair points = contactPointPair(bodies, work, contact, contactIndex, k);
        const ContactAnchor& anchor = work.contactAnchor(contactIndex, k);
        const vec3 displacement = (points.pA - points.pB) - anchor.startSeparation;
        const vec3 tangential = displacement - n * displacement.dot(n);
        const f32 tangentialLength = tangential.length();
        if (tangentialLength <= 1e-9f) {
            continue;
        }
        const vec3 t = tangential * (1.f / tangentialLength);
        const vec3 rA = points.pA - bodies.predictedPositions[A.index];
        const vec3 rB = points.pB - bodies.predictedPositions[B.index];
        const f32 wTangent =
            generalizedInverseMass(A.invMass, bodies.predictedOrientations[A.index], A.invInertia, rA, t) +
            generalizedInverseMass(B.invMass, bodies.predictedOrientations[B.index], B.invInertia, rB, t);
        const f32 pointLambda = work.contactPointLambdaValue(contactIndex, k);
        if (wTangent < 1e-10f || tangentialLength > staticCoeff * pointLambda * wTangent) {
            continue;
        }
        applyPositionalImpulse(bodies, A, B, rA, rB, t * (-tangentialLength / wTangent));
    }
    return worst;
}

f32 measureConstraintResidual(const RigidBodySoA& bodies,
                              const SolverWorkBuffers& work,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 invMassFilter(const RigidBodySoA&, u32)) {
    f32 maxViolation = 0.f;

    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        const f32 invMassA = invMassFilter(bodies, contact.bodyA);
        const f32 invMassB = invMassFilter(bodies, contact.bodyB);
        if (invMassA + invMassB < 1e-10f) {
            continue;
        }
        for (u32 k = 0; k < contact.pointCount; ++k) {
            const ContactPointPair points = contactPointPair(bodies, work, contact, contactIndex, k);
            const f32 separation = (points.pA - points.pB).dot(contact.contactNormal);
            if (separation < 0.f) {
                maxViolation = std::max(maxViolation, -separation);
            }
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
