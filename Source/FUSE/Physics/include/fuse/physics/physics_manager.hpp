#pragma once

#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/destruction/voxel_destruction.hpp>
#include <fuse/physics/events/collision_events.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::physics {

struct BhParams {
    f32 theta = 0.5f;
    u32 maxDepth = 8;
};

struct PhysicsManagerDesc {
    u32 maxBodies = 65536;
    u32 maxContacts = 262144;
    u32 maxConstraints = 131072;
    SolverParams solver{};
    BhParams nbody{};
    bool enableCcd = true;
    bool enableNbody = false;
    bool enableDestruction = true;
};

struct PhysicsStreamManager {
    void* physicsStream = nullptr;
};

/// B4.9 — ECS ↔ SoA bridge scaffold (composes with B4.1 RigidBodySoA).
class PhysicsManager {
public:
    void init(const PhysicsManagerDesc& desc);
    void destroy();

    void step(PhysicsRegistry& registry, f32 dt, PhysicsStreamManager& streams);

    bool rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal, f32& t) const;
    void querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const;
    bool isSleeping(fuse::ecs::EntityID id) const;

    void applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint = {});
    void applyForce(fuse::ecs::EntityID id, vec3 force);
    void setVelocity(fuse::ecs::EntityID id, vec3 linear, vec3 angular = {});
    void setKinematicTarget(fuse::ecs::EntityID id, vec3 position, quat orientation);

    void pushDestructionEvent(const DestructionEvent& event);

    CollisionEventSystem& collisionEvents() { return m_collisionEvents_; }
    const PhysicsManagerDesc& desc() const { return m_desc; }
    u32 stepCount() const { return m_stepCount; }
    u32 pendingDestructionEvents() const { return static_cast<u32>(m_destructionEvents_.size()); }
    u32 lastCcdHitCount() const { return m_lastCcdHitCount_; }
    const ToiBufferSoA& lastCcdBuffer() const { return m_toiBuffer_; }
    const RigidBodySoA& bodies() const { return m_soa_; }
    const CollisionShapeSoA& shapes() const { return m_shapes_; }

private:
    void syncEcsToSoa_(PhysicsRegistry& registry);
    void syncSoaToEcs_(PhysicsRegistry& registry);
    void runCcdSweep_(f32 dt);
    void processDestructionEvents_(PhysicsRegistry& registry, PhysicsResourceManager& resources);

    PhysicsManagerDesc m_desc{};
    RigidBodySoA m_soa_{};
    CollisionShapeSoA m_shapes_{};
    PBDSolver m_solver_{};
    CcdPipeline m_ccdPipeline_{};
    ToiBufferSoA m_toiBuffer_{};
    CollisionEventSystem m_collisionEvents_{};
    std::vector<fuse::ecs::EntityID> m_bodyToEntity_{};
    std::unordered_map<u32, u32> m_entityToBodyIdx_{};
    std::vector<DestructionEvent> m_destructionEvents_{};
    PhysicsResourceManager m_resources_{};
    u32 m_stepCount = 0;
    u32 m_lastCcdHitCount_ = 0;
    bool m_initialized = false;
};

} // namespace fuse::physics
