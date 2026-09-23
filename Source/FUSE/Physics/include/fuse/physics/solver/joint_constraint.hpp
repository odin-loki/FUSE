#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <limits>

namespace fuse::physics {

/// Body index meaning "the world": a fixed frame at the origin with identity orientation, so a
/// joint's B-side anchor, axis and normal are given in world space.
constexpr u32 kJointWorldBody = 0xFFFFFFFFu;

enum class JointType : u8 {
    /// Anchors coincide; optional swing cone and twist limits about the twist axis.
    BallSocket,
    /// Anchors coincide and the axes stay aligned; optional min/max angle about the axis.
    Hinge,
    /// Anchors coincide and the relative orientation stays at its creation value.
    Fixed,
    /// Anchor distance kept within [minDistance, maxDistance] (equal: rigid rod; min 0: rope).
    Distance,
    /// Soft distance: restLength with stiffness (compliance = 1 / stiffness) and damping.
    Spring,
};

/// Solver-level joint (B4.4 step 2b): XPBD positional + angular constraints between two bodies'
/// frames. Everything is in body space; the joint frame on each body is (anchor, axis, normal)
/// with `normal` perpendicular to `axis` (the zero reference for hinge angles and twist).
struct JointConstraint {
    JointType type = JointType::BallSocket;
    u32 bodyA = 0;
    u32 bodyB = kJointWorldBody;
    vec3 localAnchorA{};
    vec3 localAnchorB{};
    /// Hinge axis / twist axis (unit, body frame).
    vec3 localAxisA{0.f, 1.f, 0.f};
    vec3 localAxisB{0.f, 1.f, 0.f};
    /// Reference direction perpendicular to the axis (unit, body frame).
    vec3 localNormalA{1.f, 0.f, 0.f};
    vec3 localNormalB{1.f, 0.f, 0.f};
    /// Fixed joint: target conj(qA) * qB.
    quat restRelative{};

    /// Hinge angle limits (radians, B relative to A about the axis, right-handed).
    bool hingeLimit = false;
    f32 minAngle = 0.f;
    f32 maxAngle = 0.f;
    /// Ball-socket swing cone half-angle (radians) between the two twist axes; < 0 = free.
    f32 swingLimit = -1.f;
    /// Ball-socket twist limits (radians) about the twist axis.
    bool twistLimit = false;
    f32 minTwist = 0.f;
    f32 maxTwist = 0.f;

    /// Distance joint bounds (metres).
    f32 minDistance = 0.f;
    f32 maxDistance = 0.f;
    /// Spring rest length (metres), stiffness via `compliance`, damping (N s / m).
    f32 restLength = 0.f;
    f32 damping = 0.f;

    /// Positional compliance (m / N) and angular compliance (rad / N m); 0 = rigid.
    f32 compliance = 0.f;
    f32 angularCompliance = 0.f;

    /// Constraint force / torque above which the joint breaks (checked per substep).
    f32 breakForce = std::numeric_limits<f32>::infinity();
    f32 breakTorque = std::numeric_limits<f32>::infinity();
    /// false: contacts between the two jointed bodies are ignored (ragdolls, chains).
    bool collideConnected = false;
};

/// Per-joint output of the last `PBDSolver::step`.
struct JointSolveResult {
    /// Largest constraint force / torque over the step's substeps (N, N m).
    f32 force = 0.f;
    f32 torque = 0.f;
    /// Set once a threshold is exceeded; a broken joint is no longer solved.
    bool broken = false;
    /// Broke during the last step.
    bool brokeThisStep = false;
};

/// Per-substep XPBD state of one joint (lambdas reset each substep).
struct JointSubstepState {
    f32 lambdaPosition = 0.f;
    f32 lambdaAlign = 0.f;
    f32 lambdaLimit = 0.f;
    f32 lambdaTwist = 0.f;
    vec3 linearImpulse{};  // summed positional impulse (N s * h), for the break force
    vec3 angularImpulse{}; // summed angular impulse, for the break torque
};

/// Body data the joint solve reads: effective inverse mass (0 = immovable) and body-frame
/// inverse inertia. `index` may be kJointWorldBody.
struct JointBody {
    u32 index = kJointWorldBody;
    f32 invMass = 0.f;
    vec3 invInertia{};
};

/// One Gauss-Seidel pass over every constraint of the joint, on the predicted poses. `h` is the
/// substep length (XPBD alpha~ = compliance / h^2).
void solveJointConstraint(RigidBodySoA& bodies,
                          const JointConstraint& joint,
                          const JointBody& bodyA,
                          const JointBody& bodyB,
                          f32 h,
                          JointSubstepState& state);

/// Signed hinge angle of B relative to A about the (A) axis at the current (not predicted) poses.
f32 jointHingeAngle(const RigidBodySoA& bodies, const JointConstraint& joint);
/// Swing angle between the twist axes and twist angle about them at the current poses.
void jointSwingTwist(const RigidBodySoA& bodies, const JointConstraint& joint, f32& swing, f32& twist);

} // namespace fuse::physics
