#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Per-body positional correction accumulator — job workers write disjoint slots
/// (one writer per body per island pass) so parallel constraint phases avoid races.
struct PositionDelta {
    vec3 delta{};
    u32 writeCount = 0;
};

/// Reusable scratch for PBD constraint iterations (B4.4 deepen).
/// Avoids per-substep heap churn and holds job-safe delta slots for future CUDA parity.
struct SolverWorkBuffers {
    void init(u32 maxBodies, u32 maxContacts, u32 maxConstraints);
    void clear();

    void clearPositionDeltas();
    /// Clear a single body slot (job-safe when islands partition bodies).
    void clearPositionDeltaForBody(u32 bodyIndex);
    /// Clear only the slots touched by a constraint pair (job-safe across parallel islands).
    void clearPositionDeltasForBodies(u32 bodyA, u32 bodyB);
    /// Clear all body slots listed in an island before sequential constraint passes.
    void clearPositionDeltasForIslandBodies(const std::vector<u32>& bodyIndices);
    /// Apply accumulated deltas to predicted positions and reset touched slots.
    void applyPositionDeltas(RigidBodySoA& bodies);

    void ensureLambdaCapacity(u32 contactCount, u32 distanceCount);
    void clearLambdas();
    /// Seed contact lambda from narrowphase warm-start impulse stub (XPBD warm-start).
    void seedContactLambdaFromImpulse(u32 contactIndex, f32 warmNormalImpulse, f32 dt);
    /// Copy prior distance lambda when slot is still cold (warm-start across substeps/frames).
    void seedDistanceLambda(u32 distanceIndex, f32 priorLambda);
    f32& contactLambda(u32 contactIndex);
    f32& distanceLambda(u32 distanceIndex);

    u32 bodyCapacity() const { return static_cast<u32>(positionDeltas_.size()); }
    u32 contactCapacity() const { return static_cast<u32>(contactManifolds_.capacity()); }

    std::vector<narrowphase::ContactManifold>& contactManifolds() { return contactManifolds_; }
    const std::vector<narrowphase::ContactManifold>& contactManifolds() const { return contactManifolds_; }

    std::vector<PositionDelta>& positionDeltas() { return positionDeltas_; }
    const std::vector<PositionDelta>& positionDeltas() const { return positionDeltas_; }

    const std::vector<f32>& contactLambdas() const { return contactLambdas_; }
    const std::vector<f32>& distanceLambdas() const { return distanceLambdas_; }

private:
    std::vector<PositionDelta> positionDeltas_;
    std::vector<narrowphase::ContactManifold> contactManifolds_;
    std::vector<f32> contactLambdas_;
    std::vector<f32> distanceLambdas_;
    u32 maxConstraints_ = 0;
};

} // namespace fuse::physics
