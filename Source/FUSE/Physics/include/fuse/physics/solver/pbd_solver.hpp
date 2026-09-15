#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/contact_island_graph.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>

#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

struct SolverParams {
    u32 iterations = 10;
    u32 substeps = 4;
    vec3 gravity{0.f, -9.81f, 0.f};
    f32 linearDamping = 0.98f;
    f32 angularDamping = 0.95f;
    f32 contactCompliance = 0.f;
    f32 sleepLinearThreshold = 0.01f;
    f32 sleepAngularThreshold = 0.01f;
    f32 sleepTimeRequired = 0.5f;
    broadphase::SpatialHashParams broadphase{};
};

/// B4.4 — CPU PBD/XPBD constraint solver wired to B4.2 broadphase + B4.3 narrowphase.
class PBDSolver {
public:
    void init(u32 maxBodies, u32 maxContacts, u32 maxConstraints);
    void destroy();

    void step(RigidBodySoA& bodies,
              const CollisionShapeSoA& shapes,
              const SolverParams& params,
              f32 dt);

    void setDistanceConstraints(const std::vector<DistanceConstraint>& constraints);

    u32 contactCount() const { return lastContactCount_; }
    u32 activeBodyCount() const { return lastActiveCount_; }
    u32 lastIterationCount() const { return lastIterationCount_; }
    const ContactIslandGraph& islandGraph() const { return islandGraph_; }
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
    void resolveContact(RigidBodySoA& bodies,
                        const narrowphase::ContactManifold& contact,
                        const SolverParams& params,
                        f32 dt);
    void resolveDistanceConstraint(RigidBodySoA& bodies,
                                   const DistanceConstraint& constraint,
                                   f32 dt);
    void updateVelocities(RigidBodySoA& bodies, f32 dt);
    void applyDamping(RigidBodySoA& bodies, const SolverParams& params);
    void detectSleep(RigidBodySoA& bodies, const SolverParams& params, f32 dt);

    std::vector<DistanceConstraint> distanceConstraints_;
    SolverWorkBuffers workBuffers_;
    ContactIslandGraph islandGraph_;
    u32 maxBodies_ = 0;
    u32 maxContacts_ = 0;
    u32 lastContactCount_ = 0;
    u32 lastActiveCount_ = 0;
    u32 lastIterationCount_ = 0;
};

} // namespace fuse::physics
