#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Job-safe distance/spring XPBD correction into per-body `PositionDelta` slots.
/// Returns absolute constraint violation (|distance - restLength|) for residual stub.
/// Accumulates `lambda` across iterations/substeps when present (CPU warm-start stub).
f32 accumulateDistanceSpringCorrection(const RigidBodySoA& bodies,
                                       const DistanceConstraint& constraint,
                                       f32 invMassA,
                                       f32 invMassB,
                                       f32 dt,
                                       f32& lambda,
                                       std::vector<PositionDelta>& positionDeltas);

/// Job-safe contact normal + friction stub into per-body `PositionDelta` slots.
/// Returns penetration depth magnitude when violated, else 0.
/// Warm-starts from `lambda` when non-zero.
f32 accumulateContactCorrection(const RigidBodySoA& bodies,
                                const narrowphase::ContactManifold& contact,
                                f32 invMassA,
                                f32 invMassB,
                                f32 dt,
                                f32 contactCompliance,
                                f32& lambda,
                                std::vector<PositionDelta>& positionDeltas);

/// Max absolute constraint violation across contacts + distance springs (residual stub).
f32 measureConstraintResidual(const RigidBodySoA& bodies,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 invMassFilter(const RigidBodySoA&, u32));

} // namespace fuse::physics
