#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>

#include <fuse/types.hpp>

#include <utility>
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

struct DistanceConstraint {
    u32 bodyA = 0;
    u32 bodyB = 0;
    vec3 localAnchorA{};
    vec3 localAnchorB{};
    f32 restLength = 0.f;
    f32 compliance = 0.f;
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

private:
    void syncCollisionSnapshot_(const RigidBodySoA& bodies);
    void generateContacts(const RigidBodySoA& bodies,
                          const CollisionShapeSoA& shapes,
                          const SolverParams& params);
    void predict(RigidBodySoA& bodies, const SolverParams& params, f32 dt);
    void resolveContacts(RigidBodySoA& bodies,
                         const std::vector<narrowphase::ContactManifold>& manifolds,
                         const SolverParams& params,
                         f32 dt);
    void resolveDistanceConstraints(RigidBodySoA& bodies, f32 dt);
    void updateVelocities(RigidBodySoA& bodies, f32 dt);
    void applyDamping(RigidBodySoA& bodies, const SolverParams& params);
    void detectSleep(RigidBodySoA& bodies, const SolverParams& params, f32 dt);

    std::vector<DistanceConstraint> distanceConstraints_;
    std::vector<narrowphase::ContactManifold> contactManifolds_;
    RigidBodySoA collisionSnapshot_;
    std::vector<vec3> positionSnapshot_;
    std::vector<vec3> jacobiDeltas_;
    u32 maxBodies_ = 0;
    u32 maxContacts_ = 0;
    u32 maxConstraints_ = 0;
    u32 lastContactCount_ = 0;
    u32 lastActiveCount_ = 0;
};

} // namespace fuse::physics
