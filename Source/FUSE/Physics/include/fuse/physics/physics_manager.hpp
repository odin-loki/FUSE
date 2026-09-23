#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/destruction/voxel_destruction.hpp>
#include <fuse/physics/events/collision_events.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <limits>
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

/// Generational handle to a joint owned by the PhysicsManager.
struct JointHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const JointHandle& other) const { return index == other.index && generation == other.generation; }
    bool operator!=(const JointHandle& other) const { return !(*this == other); }
};

/// Joint between two ECS entities (or an entity and the world), given in WORLD space at creation:
/// the anchors, axis and normal are fixed into each body's frame from the entities' current
/// Transforms, so angles are measured from the creation pose (hinge angle 0, twist 0).
struct JointDesc {
    JointType type = JointType::BallSocket;
    fuse::ecs::EntityID entityA{};
    /// Null: jointed to the world.
    fuse::ecs::EntityID entityB{};
    /// Pivot (ball-socket / hinge / fixed) or anchor on A (distance / rope / spring).
    vec3 anchorA{};
    /// Anchor on B (distance / rope / spring only).
    vec3 anchorB{};
    /// Hinge axis / ball-socket twist axis (the swing cone is centred on it).
    vec3 axis{0.f, 1.f, 0.f};
    /// Zero-angle reference perpendicular to the axis (made perpendicular; any if degenerate).
    vec3 normal{1.f, 0.f, 0.f};

    bool hingeLimit = false;
    f32 minAngle = 0.f;
    f32 maxAngle = 0.f;
    /// Swing cone half-angle (radians); < 0 = free.
    f32 swingLimit = -1.f;
    bool twistLimit = false;
    f32 minTwist = 0.f;
    f32 maxTwist = 0.f;

    /// Distance / rope bounds; < 0 = the anchors' distance at creation.
    f32 minDistance = -1.f;
    f32 maxDistance = -1.f;
    /// Spring rest length (< 0 = distance at creation), stiffness (N/m) and damping (N s/m).
    f32 restLength = -1.f;
    f32 stiffness = 0.f;
    f32 damping = 0.f;

    /// Softness of the positional / angular constraints (m/N, rad/(N m)); 0 = rigid.
    f32 compliance = 0.f;
    f32 angularCompliance = 0.f;
    f32 breakForce = std::numeric_limits<f32>::infinity();
    f32 breakTorque = std::numeric_limits<f32>::infinity();
    bool collideConnected = false;

    static JointDesc ballSocket(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot, vec3 twistAxis = {0.f, 1.f, 0.f});
    static JointDesc hinge(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot, vec3 axis);
    static JointDesc fixed(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot);
    /// Rigid rod when minLength == maxLength (< 0: current distance).
    static JointDesc distance(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB,
                              f32 minLength = -1.f, f32 maxLength = -1.f);
    /// Slack below maxLength, taut at it.
    static JointDesc rope(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB, f32 maxLength = -1.f);
    static JointDesc spring(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB, f32 stiffness,
                            f32 damping = 0.f, f32 restLength = -1.f);
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

    /// Nearest shape hit along the ray (triggers ignored). Shapes reflect the last step, so an
    /// entity destroyed since still has a body until the next `step`; pass `aliveIn` (the
    /// registry) to skip entities that are no longer alive in it.
    bool rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal, f32& t,
                 const fuse::ecs::Registry* aliveIn = nullptr) const;
    /// Entities whose shapes overlap the sphere (triggers included); `aliveIn` as for rayCast.
    void querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results,
                     const fuse::ecs::Registry* aliveIn = nullptr) const;
    bool isSleeping(fuse::ecs::EntityID id) const;

    /// Instant velocity change `impulse / mass` through the centre of mass (applied to the solver
    /// state, visible in the RigidBody component after the next step). Wakes the body.
    void applyImpulse(fuse::ecs::EntityID id, vec3 impulse);
    /// Impulse at a world-space point: dv = J / m and dw = I^-1 ((worldPoint - centre) x J), with
    /// the world inverse inertia of the body's shape at its current orientation. Wakes the body.
    void applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint);
    /// Force applied over the next step.
    void applyForce(fuse::ecs::EntityID id, vec3 force);
    /// Torque (world space) applied over the next step.
    void applyTorque(fuse::ecs::EntityID id, vec3 torque);
    void setVelocity(fuse::ecs::EntityID id, vec3 linear, vec3 angular = {});
    /// Kinematic pose to reach by the end of the next step (the body is made kinematic).
    void setKinematicTarget(fuse::ecs::EntityID id, vec3 position, quat orientation);
    void pushDestructionEvent(const DestructionEvent& event);
    /// Registers the voxels that destruction events targeting `entity` carve.
    void addDestructible(fuse::ecs::EntityID entity, const VoxelVolume& volume, const VoxelMaterial& material);
    DestructibleVolume* destructible(fuse::ecs::EntityID entity);
    /// Debris spawned by the last step's destruction events (entities already in the registry).
    const std::vector<DebrisSpawn>& lastDebris() const { return m_lastDebris_; }

    // --- Joints (B4.4 step 2b on the ECS bridge) -------------------------------------------------
    /// Creates a joint from the entities' current Transforms; invalid handle if entityA (or a
    /// non-null entityB) is dead or has no Transform. Wakes both bodies. A joint is destroyed with
    /// either entity (or when it leaves the simulation), waking the survivor.
    JointHandle createJoint(const fuse::ecs::Registry& registry, const JointDesc& desc);
    /// Removes the joint and wakes its bodies. False for a stale handle.
    bool destroyJoint(JointHandle handle);
    bool isJointValid(JointHandle handle) const;
    /// True once the joint exceeded a break threshold (it is no longer simulated; destroy it to
    /// release the handle). A JointBreak event is raised exactly once, on the breaking step.
    bool isJointBroken(JointHandle handle) const;
    bool setHingeLimits(JointHandle handle, bool enabled, f32 minAngle, f32 maxAngle);
    bool setSwingTwistLimits(JointHandle handle, f32 swingLimit, bool twistEnabled, f32 minTwist, f32 maxTwist);
    bool setBreakThresholds(JointHandle handle, f32 breakForce, f32 breakTorque);
    /// Solver-level constraint (body-frame anchors / axes; body indices as of the last step).
    const JointConstraint* joint(JointHandle handle) const;
    /// Largest constraint force (N) / torque (N m) over the last step's substeps.
    f32 jointForce(JointHandle handle) const;
    f32 jointTorque(JointHandle handle) const;
    /// Hinge angle, swing and twist (radians) at the current poses; false for a stale handle or a
    /// body not in the simulation yet.
    bool jointAngles(JointHandle handle, f32& hingeAngle, f32& swing, f32& twist) const;
    /// Live (created, not destroyed) joints, broken ones included.
    u32 jointCount() const { return m_liveJointCount_; }

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
    struct JointRecord {
        JointConstraint constraint{};
        fuse::ecs::EntityID entityA{};
        fuse::ecs::EntityID entityB{};
        u32 generation = 1;
        bool alive = false;
        bool broken = false;
        f32 lastForce = 0.f;
        f32 lastTorque = 0.f;
    };
    JointRecord* jointRecord_(JointHandle handle);
    const JointRecord* jointRecord_(JointHandle handle) const;
    void releaseJoint_(u32 slot);
    void wakeEntity_(fuse::ecs::EntityID id);
    /// Drops joints whose entities left the simulation, maps the rest to body indices and hands
    /// them to the solver.
    void buildSolverJoints_();
    void collectJointBreaks_();

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
    std::vector<quat> m_writtenOrientations_{};
    std::vector<vec3> m_writtenAngularVelocities_{};
    std::vector<u8> m_seen_{};
    struct KinematicTarget {
        vec3 position{};
        quat orientation{};
    };
    std::unordered_map<u32, KinematicTarget> m_kinematicTargets_{};
    std::unordered_map<u64, PairKey> m_activePairs_{};
    std::unordered_map<u64, PairKey> m_currentPairs_{};
    std::vector<DestructionEvent> m_destructionEvents_{};
    std::unordered_map<u32, DestructibleVolume> m_destructibles_{};
    std::vector<DebrisSpawn> m_lastDebris_{};
    std::vector<JointRecord> m_joints_{};
    std::vector<u32> m_freeJointSlots_{};
    std::vector<JointConstraint> m_solverJoints_{};
    std::vector<u32> m_solverJointSlots_{};
    std::vector<fuse::ecs::EntityID> m_pendingWakes_{};
    std::vector<CollisionEvent> m_jointEvents_{};
    u32 m_liveJointCount_ = 0;
    u32 m_stepCount = 0;
    u32 m_lastCcdHitCount_ = 0;
    bool m_initialized = false;
};

} // namespace fuse::physics
