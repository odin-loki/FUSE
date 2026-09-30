#pragma once

// 2D rigid-body world behind PhysicsWorld2D (package G11, GAP-WORLD2D-GAMEPLAY).
//
// Step order (all loops in fixed id / key order, nothing iterates a hash container, so stepping is
// deterministic):
//   1. shape AABBs from the current poses (fattened by kAabbMargin for pair finding);
//   2. sort-and-sweep broadphase on the x axis -> candidate pair keys (shapeLo << 32 | shapeHi), sorted;
//   3. merge with the persistent contact list (sorted by key): new pairs become contacts, vanished
//      pairs end theirs;
//   4. narrowphase manifolds, feature-id matching for warm starting, touching / sensor transitions ->
//      begin / end events;
//   5. integrate velocities (gravity, forces, damping), sequential impulses for joints and contacts with
//      warm starting, integrate positions, non-linear position correction for contacts and joints;
//   6. deliver events to the listener.
// Buffers keep their capacity across steps, so a steady-state step does not allocate.

#include "collide_2d.hpp"

#include <fuse/physics/physics_world_2d.hpp>

#include <vector>

namespace fuse::physics::p2d {

struct Body2D {
    BodyType2D type = BodyType2D::Dynamic;
    bool alive = false;
    bool fixedRotation = false;
    Xf xf{};
    /// World centre of mass and angle (the solver state).
    vec2 c{0.f, 0.f};
    f32 a = 0.f;
    vec2 localCenter{0.f, 0.f};
    vec2 v{0.f, 0.f};
    f32 w = 0.f;
    vec2 force{0.f, 0.f};
    f32 torque = 0.f;
    f32 mass = 0.f;
    f32 invMass = 0.f;
    f32 inertia = 0.f;
    f32 invI = 0.f;
    f32 linearDamping = 0.f;
    f32 angularDamping = 0.f;
    f32 gravityScale = 1.f;
    u64 userData = 0u;
    u32 firstShape = kInvalidId2D;
    u32 lastShape = kInvalidId2D;
};

struct Contact2D {
    u64 key = 0u;
    /// Collision order (routine "A" first); may differ from the key order.
    u32 shapeA = kInvalidId2D;
    u32 shapeB = kInvalidId2D;
    bool sensor = false;
    bool touching = false;
    Manifold manifold{};
    f32 friction = 0.f;
    f32 restitution = 0.f;
};

struct Joint2D {
    JointType2D type = JointType2D::Revolute;
    bool alive = false;
    bool collideConnected = false;
    u32 bodyA = kInvalidId2D;
    u32 bodyB = kInvalidId2D;
    vec2 localAnchorA{0.f, 0.f};
    vec2 localAnchorB{0.f, 0.f};
    f32 referenceAngle = 0.f;
    bool enableLimit = false;
    bool enableMotor = false;
    f32 lower = 0.f;
    f32 upper = 0.f;
    f32 motorSpeed = 0.f;
    f32 maxMotor = 0.f;
    // Distance.
    f32 length = 1.f;
    f32 frequencyHz = 0.f;
    f32 dampingRatio = 0.f;
    // Prismatic.
    vec2 localXAxisA{1.f, 0.f};
    vec2 localYAxisA{0.f, 1.f};

    // Accumulated impulses (warm starting).
    vec2 pointImpulse{0.f, 0.f};
    f32 axialImpulse = 0.f;
    f32 motorImpulse = 0.f;
    f32 lowerImpulse = 0.f;
    f32 upperImpulse = 0.f;

    // Solver temporaries.
    vec2 rA{0.f, 0.f};
    vec2 rB{0.f, 0.f};
    vec2 localCenterA{0.f, 0.f};
    vec2 localCenterB{0.f, 0.f};
    f32 mA = 0.f;
    f32 mB = 0.f;
    f32 iA = 0.f;
    f32 iB = 0.f;
    Mat22 k22{};
    f32 axialMass = 0.f;
    f32 angle = 0.f;
    vec2 u{0.f, 0.f};
    f32 mass = 0.f;
    f32 gamma = 0.f;
    f32 bias = 0.f;
    vec2 axis{0.f, 0.f};
    vec2 perp{0.f, 0.f};
    f32 s1 = 0.f;
    f32 s2 = 0.f;
    f32 a1 = 0.f;
    f32 a2 = 0.f;
    f32 translation = 0.f;
    f32 lastImpulse = 0.f;
};

struct VelocityConstraintPoint {
    vec2 rA{0.f, 0.f};
    vec2 rB{0.f, 0.f};
    f32 normalImpulse = 0.f;
    f32 tangentImpulse = 0.f;
    f32 normalMass = 0.f;
    f32 tangentMass = 0.f;
    f32 velocityBias = 0.f;
};

struct ContactConstraint {
    u32 contactIndex = 0u;
    u32 bodyA = 0u;
    u32 bodyB = 0u;
    vec2 normal{0.f, 0.f};
    VelocityConstraintPoint points[2]{};
    u32 pointCount = 0u;
    f32 friction = 0.f;
    f32 restitution = 0.f;
    f32 invMassA = 0.f;
    f32 invMassB = 0.f;
    f32 invIA = 0.f;
    f32 invIB = 0.f;
    f32 radiusA = 0.f;
    f32 radiusB = 0.f;
    vec2 localCenterA{0.f, 0.f};
    vec2 localCenterB{0.f, 0.f};
};

struct BvhNode2D {
    Aabb2D box{};
    /// Inner node: left / right child indices. Leaf (left == kInvalidId2D): items [first, first + count).
    u32 left = kInvalidId2D;
    u32 right = kInvalidId2D;
    u32 first = 0u;
    u32 count = 0u;
};

class RigidWorld2D {
public:
    void clear();

    SolverSettings2D settings{};

    // Bodies.
    BodyId2D createBody(const BodyDef2D& def);
    void destroyBody(BodyId2D body);
    bool bodyValid(BodyId2D body) const { return body < m_bodies.size() && m_bodies[body].alive; }
    const Body2D& body(BodyId2D id) const { return m_bodies[id]; }
    Body2D& body(BodyId2D id) { return m_bodies[id]; }
    u32 aliveBodyCount() const { return m_aliveBodies; }
    void resetMassData(BodyId2D body);
    void setTransform(BodyId2D body, vec2 position, f32 angle);
    void setType(BodyId2D body, BodyType2D type);

    // Shapes.
    ShapeId2D addShape(BodyId2D body, const Shape2D& shape, const ShapeDef2D& def, bool reuseFreeSlot = true);
    bool shapeValid(ShapeId2D shape) const { return shape < m_shapes.size() && m_shapes[shape].alive; }
    const Shape2D& shape(ShapeId2D id) const { return m_shapes[id]; }
    Shape2D& shape(ShapeId2D id) { return m_shapes[id]; }
    u32 aliveShapeCount() const { return m_aliveShapes; }
    void markQueryDirty() { m_bvhDirty = true; }

    // Joints.
    JointId2D addJoint(const Joint2D& joint);
    void destroyJoint(JointId2D joint);
    bool jointValid(JointId2D joint) const { return joint < m_joints.size() && m_joints[joint].alive; }
    const Joint2D& joint(JointId2D id) const { return m_joints[id]; }
    Joint2D& joint(JointId2D id) { return m_joints[id]; }
    u32 aliveJointCount() const { return m_aliveJoints; }

    void step(f32 dt);

    // Queries.
    RayHit2D rayCastClosest(vec2 p1, vec2 p2, u32 maskBits, bool includeSensors) const;
    u32 rayCastAll(vec2 p1, vec2 p2, std::vector<RayHit2D>& out, u32 maskBits, bool includeSensors) const;
    bool rayCastShape(ShapeId2D shape, vec2 p1, vec2 p2, f32 maxFraction, RayHit2D& out) const;
    u32 queryAabb(const Aabb2D& box, std::vector<ShapeId2D>& out, u32 maskBits) const;
    u32 queryPoint(vec2 p, std::vector<ShapeId2D>& out, u32 maskBits) const;

    // Events.
    ContactListener2D* listener = nullptr;
    const std::vector<ContactEvent2D>& beginEvents() const { return m_beginEvents; }
    const std::vector<ContactEvent2D>& endEvents() const { return m_endEvents; }
    const std::vector<SensorEvent2D>& sensorBeginEvents() const { return m_sensorBegin; }
    const std::vector<SensorEvent2D>& sensorEndEvents() const { return m_sensorEnd; }
    const RigidStepStats2D& stats() const { return m_stats; }
    const std::vector<Contact2D>& contacts() const { return m_contacts; }
    u32 bodyContactCount(BodyId2D body) const;

private:
    struct Proxy {
        f32 minX = 0.f;
        f32 maxX = 0.f;
        u32 shape = 0u;
    };

    void updateAabbs();
    void findPairs();
    void updateContacts();
    void evaluateContact(Contact2D& contact, bool isNew);
    bool shouldCollide(u32 shapeA, u32 shapeB) const;
    void rebuildJointPairs();
    void removeDeadShapeContacts();
    void emitEnd(const Contact2D& contact);
    ContactEvent2D makeEvent(const Contact2D& contact) const;

    void solve(f32 dt);
    void initContactConstraints(f32 dtRatio);
    void warmStartContacts();
    void solveContactVelocities();
    void storeContactImpulses();
    bool solveContactPositions();

    void initJointVelocity(Joint2D& joint, f32 dt);
    void solveJointVelocity(Joint2D& joint, f32 dt);
    bool solveJointPosition(Joint2D& joint);

    void rebuildBvh() const;
    u32 buildBvhNode(u32 first, u32 count, u32 depth) const;

    std::vector<Body2D> m_bodies;
    std::vector<u32> m_freeBodies;
    std::vector<Shape2D> m_shapes;
    std::vector<u32> m_freeShapes;
    std::vector<Joint2D> m_joints;
    std::vector<u32> m_freeJoints;
    u32 m_aliveBodies = 0u;
    u32 m_aliveShapes = 0u;
    u32 m_aliveJoints = 0u;

    std::vector<Proxy> m_proxies;
    std::vector<u64> m_pairKeys;
    std::vector<u64> m_jointPairs;
    bool m_jointPairsDirty = false;
    std::vector<Contact2D> m_contacts;
    std::vector<Contact2D> m_contactsScratch;
    std::vector<ContactConstraint> m_constraints;

    std::vector<ContactEvent2D> m_beginEvents;
    std::vector<ContactEvent2D> m_endEvents;
    std::vector<SensorEvent2D> m_sensorBegin;
    std::vector<SensorEvent2D> m_sensorEnd;
    std::vector<ContactEvent2D> m_pendingEnd;
    std::vector<SensorEvent2D> m_pendingSensorEnd;
    RigidStepStats2D m_stats{};
    f32 m_prevDt = 0.f;
    f32 m_dtRatio = 1.f;

    mutable bool m_bvhDirty = true;
    mutable std::vector<BvhNode2D> m_bvhNodes;
    mutable std::vector<u32> m_bvhItems;
    mutable std::vector<Aabb2D> m_queryAabbs;
    mutable std::vector<u32> m_bvhStack;
    mutable u32 m_bvhRoot = kInvalidId2D;
};

} // namespace fuse::physics::p2d
