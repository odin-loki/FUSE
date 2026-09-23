#pragma once

#include <fuse/ecs/registry.hpp>
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

/// B4.9 — ECS <-> SoA bridge. Every entity with Transform + RigidBody + Collider is a body:
/// `step` copies game-side edits (teleports, velocity writes, forces) into the solver, runs the
/// PBD step, writes positions/velocities/sleep state back into the components and dispatches
/// Enter/Stay/Exit/Trigger events. TagKinematic bodies follow their Transform (or a
/// `setKinematicTarget`) and push dynamic bodies without being pushed back.
class PhysicsManager {
public:
    void init(const PhysicsManagerDesc& desc);
    void destroy();
    void step(fuse::ecs::Registry& registry, f32 dt, PhysicsStreamManager& streams);

    /// Nearest shape hit along the ray (triggers ignored).
    bool rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal, f32& t) const;
    /// Entities whose shapes overlap the sphere (triggers included).
    void querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const;
    bool isSleeping(fuse::ecs::EntityID id) const;

    /// Instant velocity change `impulse / mass` (applied to the solver state, visible in the
    /// RigidBody component after the next step). Wakes the body.
    void applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint = {});
    /// Force applied over the next step.
    void applyForce(fuse::ecs::EntityID id, vec3 force);
    void setVelocity(fuse::ecs::EntityID id, vec3 linear, vec3 angular = {});
    /// Kinematic pose to reach by the end of the next step (the body is made kinematic).
    void setKinematicTarget(fuse::ecs::EntityID id, vec3 position, quat orientation);
    void pushDestructionEvent(const DestructionEvent& event);
    /// Registers the voxels that destruction events targeting `entity` carve.
    void addDestructible(fuse::ecs::EntityID entity, const VoxelVolume& volume, const VoxelMaterial& material);
    DestructibleVolume* destructible(fuse::ecs::EntityID entity);
    /// Debris spawned by the last step's destruction events (entities already in the registry).
    const std::vector<DebrisSpawn>& lastDebris() const { return m_lastDebris_; }

    CollisionEventSystem& collisionEvents() { return m_collisionEvents_; }
    /// Events raised by the last step (also dispatched to the registered callbacks).
    const std::vector<CollisionEvent>& lastEvents() const { return m_lastEvents_; }
    const PhysicsManagerDesc& desc() const { return m_desc; }
    u32 stepCount() const { return m_stepCount; }
    u32 pendingDestructionEvents() const { return static_cast<u32>(m_destructionEvents_.size()); }
    u32 lastCcdHitCount() const { return m_lastCcdHitCount_; }
    const ToiBufferSoA& lastCcdBuffer() const { return m_solver_.ccdBuffer(); }
    const RigidBodySoA& bodies() const { return m_soa_; }
    const CollisionShapeSoA& shapes() const { return m_shapes_; }
    const PBDSolver& solver() const { return m_solver_; }
    /// Solver body index of an entity, or ~0u.
    u32 bodyIndex(fuse::ecs::EntityID id) const;
    fuse::ecs::EntityID entityOf(u32 bodyIndex) const { return m_bodyToEntity_[bodyIndex]; }

private:
    struct PairKey {
        fuse::ecs::EntityID a{};
        fuse::ecs::EntityID b{};
        bool trigger = false;
    };

    void syncEcsToSoa_(fuse::ecs::Registry& registry, f32 dt);
    void syncSoaToEcs_(fuse::ecs::Registry& registry);
    void removeBody_(u32 bodyIndex);
    void raiseEvents_();
    void processDestructionEvents_(fuse::ecs::Registry& registry);

    PhysicsManagerDesc m_desc{};
    RigidBodySoA m_soa_{};
    CollisionShapeSoA m_shapes_{};
    PBDSolver m_solver_{};
    CollisionEventSystem m_collisionEvents_{};
    std::vector<CollisionEvent> m_lastEvents_{};
    std::vector<fuse::ecs::EntityID> m_bodyToEntity_{};
    std::unordered_map<u32, u32> m_entityToBodyIdx_{};
    /// Values last written to the components: a difference at sync time is a game-side edit.
    std::vector<vec3> m_writtenPositions_{};
    std::vector<vec3> m_writtenVelocities_{};
    std::vector<u8> m_seen_{};
    std::unordered_map<u32, vec3> m_kinematicTargets_{};
    std::unordered_map<u64, PairKey> m_activePairs_{};
    std::unordered_map<u64, PairKey> m_currentPairs_{};
    std::vector<DestructionEvent> m_destructionEvents_{};
    std::unordered_map<u32, DestructibleVolume> m_destructibles_{};
    std::vector<DebrisSpawn> m_lastDebris_{};
    u32 m_stepCount = 0;
    u32 m_lastCcdHitCount_ = 0;
    bool m_initialized = false;
};

} // namespace fuse::physics
