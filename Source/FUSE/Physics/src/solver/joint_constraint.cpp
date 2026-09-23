#include <fuse/physics/solver/joint_constraint.hpp>

#include <fuse/physics/rotation.hpp>

#include <cmath>

namespace fuse::physics {
namespace {

bool isWorld(u32 index) {
    return index == kJointWorldBody;
}

vec3& predictedPosition(RigidBodySoA& bodies, u32 index, vec3& worldScratch) {
    return isWorld(index) ? worldScratch : bodies.predictedPositions[index];
}

quat& predictedOrientation(RigidBodySoA& bodies, u32 index, quat& worldScratch) {
    return isWorld(index) ? worldScratch : bodies.predictedOrientations[index];
}

vec3 currentPosition(const RigidBodySoA& bodies, u32 index) {
    return isWorld(index) ? vec3{} : bodies.positions[index];
}

quat currentOrientation(const RigidBodySoA& bodies, u32 index) {
    return isWorld(index) ? quat{} : bodies.orientations[index];
}

/// The joint's two body poses at the predicted state (world side is a fixed identity frame).
struct PosePair {
    vec3* pA = nullptr;
    vec3* pB = nullptr;
    quat* qA = nullptr;
    quat* qB = nullptr;
};

f32 angularInverseMass(const JointBody& body, const quat& q, vec3 n) {
    if (body.invMass <= 0.f) {
        return 0.f;
    }
    return n.dot(applyInverseInertia(q, body.invInertia, n));
}

/// XPBD positional constraint along unit `n` at arms rA / rB. `C` is the signed violation
/// (positive: the anchors must approach along n). `gamma` / `rate` add XPBD damping.
void solveLinear(const JointBody& A,
                 const JointBody& B,
                 PosePair& pose,
                 vec3 rA,
                 vec3 rB,
                 vec3 n,
                 f32 C,
                 f32 alpha,
                 f32 gamma,
                 f32 rate,
                 f32& lambda,
                 JointSubstepState& state) {
    const f32 wA = A.invMass > 0.f ? generalizedInverseMass(A.invMass, *pose.qA, A.invInertia, rA, n) : 0.f;
    const f32 wB = B.invMass > 0.f ? generalizedInverseMass(B.invMass, *pose.qB, B.invInertia, rB, n) : 0.f;
    const f32 w = wA + wB;
    if (w < 1e-10f) {
        return;
    }
    const f32 deltaLambda = (-C - alpha * lambda - gamma * rate) / ((1.f + gamma) * w + alpha);
    lambda += deltaLambda;
    const vec3 impulse = n * deltaLambda;
    state.linearImpulse += impulse;
    if (A.invMass > 0.f) {
        *pose.pA += impulse * A.invMass;
        const vec3 dTheta = applyInverseInertia(*pose.qA, A.invInertia, rA.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            *pose.qA = integrateRotation(*pose.qA, dTheta);
        }
    }
    if (B.invMass > 0.f) {
        *pose.pB -= impulse * B.invMass;
        const vec3 dTheta = applyInverseInertia(*pose.qB, B.invInertia, rB.cross(impulse));
        if (dTheta.dot(dTheta) > 0.f) {
            *pose.qB = integrateRotation(*pose.qB, dTheta * -1.f);
        }
    }
}

/// XPBD angular constraint: `correction` is the rotation vector that B must turn by relative to A.
void solveAngular(const JointBody& A,
                  const JointBody& B,
                  PosePair& pose,
                  vec3 correction,
                  f32 alpha,
                  f32& lambda,
                  JointSubstepState& state) {
    const f32 angle = correction.length();
    if (angle < 1e-9f) {
        return;
    }
    const vec3 n = correction * (1.f / angle);
    const f32 w = angularInverseMass(A, *pose.qA, n) + angularInverseMass(B, *pose.qB, n);
    if (w < 1e-10f) {
        return;
    }
    const f32 deltaLambda = (angle - alpha * lambda) / (w + alpha);
    lambda += deltaLambda;
    const vec3 impulse = n * deltaLambda;
    state.angularImpulse += impulse;
    if (B.invMass > 0.f) {
        *pose.qB = integrateRotation(*pose.qB, applyInverseInertia(*pose.qB, B.invInertia, impulse));
    }
    if (A.invMass > 0.f) {
        *pose.qA = integrateRotation(*pose.qA, applyInverseInertia(*pose.qA, A.invInertia, impulse) * -1.f);
    }
}

/// Rotation vector turning unit `from` onto unit `to` (shortest arc).
vec3 alignmentCorrection(vec3 from, vec3 to) {
    const vec3 axis = from.cross(to);
    const f32 sinAngle = axis.length();
    if (sinAngle < 1e-9f) {
        return {};
    }
    return axis * (std::atan2(sinAngle, from.dot(to)) / sinAngle);
}

/// Signed angle from `a` to `b` about unit `axis` (both projected onto the plane normal to it).
f32 signedAngleAbout(vec3 axis, vec3 a, vec3 b) {
    return std::atan2(axis.dot(a.cross(b)), a.dot(b));
}

/// Correction angle pushing `angle` back into [lo, hi] (0 inside).
f32 limitCorrection(f32 angle, f32 lo, f32 hi) {
    if (angle < lo) {
        return lo - angle;
    }
    if (angle > hi) {
        return hi - angle;
    }
    return 0.f;
}

/// Twist of b relative to a about the mean of the two twist axes (Mueller et al. 2020).
bool twistAngle(vec3 axisA, vec3 axisB, vec3 normalA, vec3 normalB, vec3& n, f32& twist) {
    n = axisA + axisB;
    const f32 len = n.length();
    if (len < 1e-4f) {
        return false; // axes opposed: twist undefined
    }
    n = n * (1.f / len);
    vec3 a = normalA - n * n.dot(normalA);
    vec3 b = normalB - n * n.dot(normalB);
    const f32 la = a.length();
    const f32 lb = b.length();
    if (la < 1e-6f || lb < 1e-6f) {
        return false;
    }
    a = a * (1.f / la);
    b = b * (1.f / lb);
    twist = signedAngleAbout(n, a, b);
    return true;
}

} // namespace

void solveJointConstraint(RigidBodySoA& bodies,
                          const JointConstraint& joint,
                          const JointBody& A,
                          const JointBody& B,
                          f32 h,
                          JointSubstepState& state) {
    if (A.invMass + B.invMass <= 0.f || h <= 0.f) {
        return;
    }
    PosePair pose{};
    vec3 worldPositionB{};
    quat worldOrientationB{};
    vec3 worldPositionA{};
    quat worldOrientationA{};
    pose.pA = &predictedPosition(bodies, joint.bodyA, worldPositionA);
    pose.qA = &predictedOrientation(bodies, joint.bodyA, worldOrientationA);
    pose.pB = &predictedPosition(bodies, joint.bodyB, worldPositionB);
    pose.qB = &predictedOrientation(bodies, joint.bodyB, worldOrientationB);
    const f32 invH2 = 1.f / (h * h);
    const f32 angularAlpha = joint.angularCompliance * invH2;

    // Angular constraints first, so the anchors end the pass exactly together.
    switch (joint.type) {
    case JointType::Hinge: {
        const vec3 axisA = rotate(*pose.qA, joint.localAxisA);
        const vec3 axisB = rotate(*pose.qB, joint.localAxisB);
        solveAngular(A, B, pose, alignmentCorrection(axisB, axisA), angularAlpha, state.lambdaAlign, state);
        if (joint.hingeLimit) {
            const vec3 axis = rotate(*pose.qA, joint.localAxisA);
            const f32 angle = signedAngleAbout(axis, rotate(*pose.qA, joint.localNormalA),
                                               rotate(*pose.qB, joint.localNormalB));
            const f32 correction = limitCorrection(angle, joint.minAngle, joint.maxAngle);
            if (correction != 0.f) {
                solveAngular(A, B, pose, axis * correction, angularAlpha, state.lambdaLimit, state);
            }
        }
        break;
    }
    case JointType::BallSocket: {
        if (joint.swingLimit >= 0.f) {
            const vec3 axisA = rotate(*pose.qA, joint.localAxisA);
            const vec3 axisB = rotate(*pose.qB, joint.localAxisB);
            const vec3 toA = alignmentCorrection(axisB, axisA); // |toA| = swing angle
            const f32 swing = toA.length();
            if (swing > joint.swingLimit) {
                solveAngular(A, B, pose, toA * ((swing - joint.swingLimit) / swing), angularAlpha, state.lambdaLimit,
                             state);
            }
        }
        if (joint.twistLimit) {
            vec3 n{};
            f32 twist = 0.f;
            if (twistAngle(rotate(*pose.qA, joint.localAxisA), rotate(*pose.qB, joint.localAxisB),
                           rotate(*pose.qA, joint.localNormalA), rotate(*pose.qB, joint.localNormalB), n, twist)) {
                const f32 correction = limitCorrection(twist, joint.minTwist, joint.maxTwist);
                if (correction != 0.f) {
                    solveAngular(A, B, pose, n * correction, angularAlpha, state.lambdaTwist, state);
                }
            }
        }
        break;
    }
    case JointType::Fixed: {
        // Target qB = qA * rest; correction = log(target * conj(qB)) in world space.
        quat delta = quatMul(quatMul(*pose.qA, joint.restRelative), quatConjugate(*pose.qB));
        if (delta.w < 0.f) {
            delta = {-delta.x, -delta.y, -delta.z, -delta.w};
        }
        const f32 sinHalf = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (sinHalf > 1e-9f) {
            const f32 scale = 2.f * std::atan2(sinHalf, delta.w) / sinHalf;
            solveAngular(A, B, pose, vec3{delta.x, delta.y, delta.z} * scale, angularAlpha, state.lambdaAlign,
                         state);
        }
        break;
    }
    default:
        break;
    }

    // Positional part at the anchors.
    const vec3 rA = rotate(*pose.qA, joint.localAnchorA);
    const vec3 rB = rotate(*pose.qB, joint.localAnchorB);
    const vec3 diff = (*pose.pA + rA) - (*pose.pB + rB);
    const f32 distance = diff.length();
    const f32 alpha = joint.compliance * invH2;
    switch (joint.type) {
    case JointType::BallSocket:
    case JointType::Hinge:
    case JointType::Fixed:
        if (distance > 1e-9f) {
            solveLinear(A, B, pose, rA, rB, diff * (1.f / distance), distance, alpha, 0.f, 0.f, state.lambdaPosition,
                        state);
        }
        break;
    case JointType::Distance: {
        if (distance < 1e-9f) {
            break;
        }
        f32 C = 0.f;
        if (distance < joint.minDistance) {
            C = distance - joint.minDistance;
        } else if (distance > joint.maxDistance) {
            C = distance - joint.maxDistance;
        }
        if (C != 0.f) {
            solveLinear(A, B, pose, rA, rB, diff * (1.f / distance), C, alpha, 0.f, 0.f, state.lambdaPosition, state);
        }
        break;
    }
    case JointType::Spring: {
        if (distance < 1e-9f) {
            break;
        }
        const vec3 n = diff * (1.f / distance);
        // XPBD damping (Macklin et al. 2016): gamma = alpha~ beta~ / h with beta~ = h^2 damping.
        const f32 gamma = alpha > 0.f ? joint.compliance * joint.damping / h : 0.f;
        f32 rate = 0.f;
        if (gamma > 0.f) {
            const vec3 startA = currentPosition(bodies, joint.bodyA) +
                                rotate(currentOrientation(bodies, joint.bodyA), joint.localAnchorA);
            const vec3 startB = currentPosition(bodies, joint.bodyB) +
                                rotate(currentOrientation(bodies, joint.bodyB), joint.localAnchorB);
            rate = n.dot(((*pose.pA + rA) - startA) - ((*pose.pB + rB) - startB));
        }
        solveLinear(A, B, pose, rA, rB, n, distance - joint.restLength, alpha, gamma, rate, state.lambdaPosition,
                    state);
        break;
    }
    }
}

f32 jointHingeAngle(const RigidBodySoA& bodies, const JointConstraint& joint) {
    const quat qA = currentOrientation(bodies, joint.bodyA);
    const quat qB = currentOrientation(bodies, joint.bodyB);
    return signedAngleAbout(rotate(qA, joint.localAxisA), rotate(qA, joint.localNormalA),
                            rotate(qB, joint.localNormalB));
}

void jointSwingTwist(const RigidBodySoA& bodies, const JointConstraint& joint, f32& swing, f32& twist) {
    const quat qA = currentOrientation(bodies, joint.bodyA);
    const quat qB = currentOrientation(bodies, joint.bodyB);
    const vec3 axisA = rotate(qA, joint.localAxisA);
    const vec3 axisB = rotate(qB, joint.localAxisB);
    swing = alignmentCorrection(axisB, axisA).length();
    vec3 n{};
    twist = 0.f;
    twistAngle(axisA, axisB, rotate(qA, joint.localNormalA), rotate(qB, joint.localNormalB), n, twist);
}

} // namespace fuse::physics
