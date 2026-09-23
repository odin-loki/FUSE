#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Job-safe distance/spring XPBD correction into per-body `PositionDelta` slots (translation only:
/// the anchors follow the predicted orientations but no torque is applied; the solver itself uses
/// `solveDistanceConstraint`).
/// Returns absolute constraint violation (|distance - restLength|) for residual stub.
/// Accumulates `lambda` across iterations/substeps when present (CPU warm-start stub).
f32 accumulateDistanceSpringCorrection(const RigidBodySoA& bodies,
                                       const DistanceConstraint& constraint,
                                       f32 invMassA,
                                       f32 invMassB,
                                       f32 dt,
                                       f32& lambda,
                                       std::vector<PositionDelta>& positionDeltas);

/// One side of a contact constraint: body index and its effective inverse mass / body-frame
/// diagonal inverse inertia (both zero for static, kinematic or sleeping bodies).
struct ContactBody {
    u32 index = 0;
    f32 invMass = 0.f;
    vec3 invInertia{};
};

/// Anchored distance constraint (joint) solve: the anchors are fixed in each body's frame, so the
/// correction uses the generalized inverse masses at the anchors, w = 1/m + (r x n)^T I^-1 (r x n),
/// and moves and rotates both predicted poses directly. restLength 0 is a ball-socket joint.
/// Accumulates `lambda`; returns |distance - restLength| before the correction.
f32 solveDistanceConstraint(RigidBodySoA& bodies,
                            const DistanceConstraint& constraint,
                            const ContactBody& bodyA,
                            const ContactBody& bodyB,
                            f32 dt,
                            f32& lambda);

/// XPBD contact solve on every manifold point: the points are fixed in both bodies' frames
/// (`SolverWorkBuffers::prepareContactPoints`), so the correction uses the generalized inverse
/// masses w = 1/m + (r x n)^T I^-1 (r x n) and moves and rotates both bodies' predicted poses
/// directly. Static friction cancels the points' tangential drift inside the friction cone.
/// Accumulates the manifold `lambda` and per-point lambdas; returns the deepest violation.
f32 solveContactConstraint(RigidBodySoA& bodies,
                           SolverWorkBuffers& work,
                           u32 contactIndex,
                           const narrowphase::ContactManifold& contact,
                           const ContactBody& bodyA,
                           const ContactBody& bodyB,
                           f32 dt,
                           f32 contactCompliance,
                           f32& lambda);

/// Max absolute constraint violation across contacts + distance springs (residual stub).
f32 measureConstraintResidual(const RigidBodySoA& bodies,
                              const SolverWorkBuffers& work,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 invMassFilter(const RigidBodySoA&, u32));

} // namespace fuse::physics
