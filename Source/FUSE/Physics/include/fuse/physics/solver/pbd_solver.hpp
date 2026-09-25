#pragma once

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/core/flat_u64_map.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/constraint_coloring.hpp>
#include <fuse/physics/solver/contact_island_graph.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/joint_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>

#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::physics {

struct SolverParams {
    u32 iterations = 10;
    u32 substeps = 4;
    vec3 gravity{0.f, -9.81f, 0.f};
    f32 linearDamping = 0.98f;
    f32 angularDamping = 0.95f;
    f32 contactCompliance = 0.f;
    /// Speculative contact distance: shapes closer than this get (inactive) contacts, so a body
    /// pushed into its neighbour during the iterations is caught in the same substep. Resting
    /// stacks rely on it; only penetrating contacts count as touching (events, contactCount).
    f32 contactMargin = 0.005f;
    f32 sleepLinearThreshold = 0.01f;
    f32 sleepAngularThreshold = 0.01f;
    f32 sleepTimeRequired = 0.5f;
    /// Max constraint violation (residual stub) for early exit; 0 disables.
    f32 residualTolerance = 0.f;
    broadphase::SpatialHashParams broadphase{};
    /// Sweep RB_CCD bodies over the frame before the discrete substeps (B4.6).
    bool enableCcd = true;
    /// Constraint iteration order. IslandGaussSeidel (default) is the reference solver;
    /// ColoredKernel graph-colours the constraints and runs one "physics_solve_color" kernel launch
    /// per colour and iteration (deterministic on every backend, but a different Gauss-Seidel order,
    /// so not bit-identical to the island path). See constraint_coloring.hpp.
    ConstraintSolveMode solveMode = ConstraintSolveMode::IslandGaussSeidel;
    /// Backend of the colored solve launches (CPU backends; GPU requests fall back to CpuParallel).
    kernel::Backend kernelBackend = kernel::Backend::CpuParallel;
};

/// B4.4 — CPU PBD/XPBD constraint solver wired to B4.2 broadphase + B4.3 narrowphase.
/// A body pair that touched during the last step (any substep), for collision events.
struct FrameContact {
    u32 bodyA = 0;
    u32 bodyB = 0;
    vec3 normal{};   // B -> A
    vec3 point{};
    f32 impulse = 0.f; // summed normal impulse over the step
    bool trigger = false;
};

class PBDSolver {
public:
    void init(u32 maxBodies, u32 maxContacts, u32 maxConstraints);
    void destroy();

    void step(RigidBodySoA& bodies,
              const CollisionShapeSoA& shapes,
              const SolverParams& params,
              f32 dt);

    void setDistanceConstraints(const std::vector<DistanceConstraint>& constraints);
    /// Joints (ball-socket / hinge / fixed / distance / spring with angle limits), solved after the
    /// contacts and distance constraints in every iteration. Resets every joint's broken state.
    void setJoints(const std::vector<JointConstraint>& joints);
    const std::vector<JointConstraint>& joints() const { return joints_; }
    /// Per-joint force / torque / break state from the last step (same order as `setJoints`).
    const std::vector<JointSolveResult>& jointResults() const { return jointResults_; }

    /// B4.6 CCD: sweeps each awake RB_CCD body against every shape its frame motion can
    /// reach, advances it to the earliest time of impact and reflects its approach velocity
    /// (restitution applied) so the discrete substeps cannot tunnel. Returns bodies clamped.
    u32 applyContinuousCollision(RigidBodySoA& bodies, const CollisionShapeSoA& shapes, f32 dt);
    /// Pairs that touched (or overlapped, for triggers) during the last step.
    const std::vector<FrameContact>& frameContacts() const { return frameContacts_; }
    u32 lastCcdHitCount() const { return lastCcdHitCount_; }
    const ToiBufferSoA& ccdBuffer() const { return ccdBuffer_; }

    u32 contactCount() const { return lastContactCount_; }
    u32 activeBodyCount() const { return lastActiveCount_; }
    u32 lastIterationCount() const { return lastIterationCount_; }
    f32 lastConstraintResidual() const { return lastConstraintResidual_; }
    const ContactIslandGraph& islandGraph() const { return islandGraph_; }
    /// Constraint colouring of the last substep (ConstraintSolveMode::ColoredKernel only).
    const ConstraintColoring& constraintColoring() const { return coloring_; }
    const SolverWorkBuffers& workBuffers() const { return workBuffers_; }

private:
    void generateContacts(RigidBodySoA& bodies,
                          const CollisionShapeSoA& shapes,
                          const SolverParams& params);
    void predict(RigidBodySoA& bodies, const SolverParams& params, f32 dt);
    void runConstraintIterations(RigidBodySoA& bodies, const SolverParams& params, f32 dt);
    void resolveIslandConstraints(RigidBodySoA& bodies,
                                  const ContactIslandGraph::Island& island,
                                  const SolverParams& params,
                                  f32 dt);
    f32 measureConstraintResidual_(RigidBodySoA& bodies) const;
    void updateVelocities(RigidBodySoA& bodies, f32 dt);
    /// Velocity pass after each substep: dynamic friction and restitution from the
    /// substep's normal impulses and pre-solve velocities (XPBD rigid bodies, sec. 3.6).
    void solveVelocities(RigidBodySoA& bodies, const SolverParams& params, f32 dt);
    void applyDamping(RigidBodySoA& bodies, const SolverParams& params);
    void detectSleep(RigidBodySoA& bodies, const SolverParams& params, f32 dt);

    std::vector<DistanceConstraint> distanceConstraints_;
    std::vector<JointConstraint> joints_;
    std::vector<JointSolveResult> jointResults_;
    std::vector<JointSubstepState> jointStates_;
    /// Sorted (lo << 32 | hi) body pairs whose contacts are ignored (collideConnected == false).
    std::vector<u64> jointIgnoredPairs_;
    void solveJoints_(RigidBodySoA& bodies, f32 dt);
    void finishJointSubstep_(f32 dt);
    bool jointIgnoresPair_(u32 a, u32 b) const;
    void recordFrameContacts_(RigidBodySoA& bodies, const SolverParams& params);
    void wakeJointedBodies_(RigidBodySoA& bodies, const SolverParams& params);
    u32 slotForFrameContact_(const narrowphase::ContactManifold& manifold, bool trigger);

    void mapBodyShapes_(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);
    /// Body-frame inverse inertia per body from its shape and mass (zero for RB_FIXED_ROTATION).
    void computeInverseInertia_(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

    std::vector<vec3> preSolveVelocities_;
    std::vector<vec3> preSolveAngular_;
    std::vector<narrowphase::ContactManifold> triggerManifolds_;
    broadphase::GridBroadphase grid_;
    std::vector<broadphase::CandidatePair> candidatePairs_;
    std::vector<narrowphase::ContactManifold> narrowManifolds_;
    std::vector<FrameContact> frameContacts_;
    /// Contact pair -> frameContacts_ slot; storage survives clear() (no per-step allocation).
    FlatU64Map<u32> frameContactSlot_;
    std::vector<u32> substepContactSlot_;
    std::vector<u32> bodyShape_;
    std::vector<aabb> sweptBounds_;
    std::vector<broadphase::CandidatePair> ccdPairs_;
    std::vector<u8> ccdHandled_;
    ToiBufferSoA ccdBuffer_;
    u32 lastCcdHitCount_ = 0;
    std::vector<f32> substepLambdaStart_;
    /// Previous step's lambdas for warm starting (members, not per-step copies: capacity reused).
    std::vector<f32> priorDistanceLambdas_;
    std::vector<f32> priorContactLambdas_;
    SolverWorkBuffers workBuffers_;
    ContactIslandGraph islandGraph_;
    ConstraintColoring coloring_;
    void solveColored_(RigidBodySoA& bodies, const SolverParams& params, f32 dt);
    u32 maxBodies_ = 0;
    u32 maxContacts_ = 0;
    u32 lastContactCount_ = 0;
    u32 lastActiveCount_ = 0;
    u32 lastIterationCount_ = 0;
    f32 lastConstraintResidual_ = 0.f;
};

} // namespace fuse::physics
