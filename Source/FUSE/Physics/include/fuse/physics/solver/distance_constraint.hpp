#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Keeps the anchor points (fixed in each body's frame, relative to the body centre) at
/// `restLength` apart. Off-centre anchors apply torque through the generalized inverse mass at the
/// anchors, so the bodies swing and rotate like a real joint. `compliance` (m/N) softens it.
struct DistanceConstraint {
    u32 bodyA = 0;
    u32 bodyB = 0;
    vec3 localAnchorA{};
    vec3 localAnchorB{};
    f32 restLength = 0.f;
    f32 compliance = 0.f;
};

/// Ball-socket joint: the two anchors coincide (a zero-length anchored distance constraint).
inline DistanceConstraint ballSocketJoint(u32 bodyA, u32 bodyB, vec3 localAnchorA, vec3 localAnchorB) {
    return DistanceConstraint{bodyA, bodyB, localAnchorA, localAnchorB, 0.f, 0.f};
}

/// Hinge joint as two ball-sockets on the hinge axis, `halfSpan` either side of the pivot. The
/// bodies keep the pivots and the axes (unit, in each body's frame) together and turn freely only
/// about the axis. Appends two constraints to `out`.
inline void appendHingeJoint(std::vector<DistanceConstraint>& out,
                             u32 bodyA,
                             u32 bodyB,
                             vec3 localPivotA,
                             vec3 localPivotB,
                             vec3 localAxisA,
                             vec3 localAxisB,
                             f32 halfSpan = 0.25f) {
    const vec3 offsetA = localAxisA.normalized() * halfSpan;
    const vec3 offsetB = localAxisB.normalized() * halfSpan;
    out.push_back(ballSocketJoint(bodyA, bodyB, localPivotA + offsetA, localPivotB + offsetB));
    out.push_back(ballSocketJoint(bodyA, bodyB, localPivotA - offsetA, localPivotB - offsetB));
}

} // namespace fuse::physics
