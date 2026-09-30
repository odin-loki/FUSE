#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <span>
#include <vector>

namespace fuse::physics {

namespace p2d {
class RigidWorld2D;
} // namespace p2d

class PhysicsWorld2D;

// ---------------------------------------------------------------------------------------------------
// 2D rigid-body API (GAP-WORLD2D-GAMEPLAY, package G11). A Box2D-feature subset written for FUSE:
// rotating bodies (mass, inertia, torque, angular velocity), circle / convex polygon / edge / chain
// shapes (SAT + reference-face clipping manifolds with feature ids), persistent contacts solved with
// sequential impulses and warm starting, Baumgarte-free non-linear position correction, contact and
// sensor begin/end events, ray casts and AABB / point queries (BVH over the shape AABBs), and revolute /
// distance / prismatic joints. Stepping is deterministic: every loop runs in a fixed order (shapes by
// id, contacts by (shapeA, shapeB) key, joints by id) and nothing iterates a hash container, so the same
// command sequence produces bit-identical state on the same build.
//
// Chains are one-sided like Box2D v2.4 chains: an edge collides only on the right-hand side of its
// direction (normal = (e.y, -e.x)), so a counter-clockwise loop faces outward and a ground strip must be
// listed right-to-left to face up. Adjacent chain vertices act as ghost vertices so shapes slide over
// interior vertices without snagging. Stand-alone edges (addEdge) are two-sided.
// ---------------------------------------------------------------------------------------------------

using BodyId2D = u32;
using ShapeId2D = u32;
using JointId2D = u32;
inline constexpr u32 kInvalidId2D = 0xFFFFFFFFu;

enum class BodyType2D : u8 { Static, Kinematic, Dynamic };
enum class ShapeType2D : u8 { Circle, Polygon, Edge };
enum class JointType2D : u8 { Revolute, Distance, Prismatic };

/// Maximum vertices of a convex polygon shape.
inline constexpr u32 kMaxPolygonVertices2D = 8u;

struct BodyDef2D {
    BodyType2D type = BodyType2D::Dynamic;
    vec2 position{0.f, 0.f};
    f32 angle = 0.f;
    vec2 linearVelocity{0.f, 0.f};
    f32 angularVelocity = 0.f;
    f32 linearDamping = 0.f;
    f32 angularDamping = 0.f;
    f32 gravityScale = 1.f;
    bool fixedRotation = false;
    u64 userData = 0u;
};

/// Collision filter: two shapes collide when (a.category & b.mask) && (b.category & a.mask), unless
/// both share a non-zero group (positive: always collide, negative: never collide).
struct Filter2D {
    u32 categoryBits = 1u;
    u32 maskBits = 0xFFFFFFFFu;
    i32 groupIndex = 0;
};

struct ShapeDef2D {
    f32 density = 1.f;
    f32 friction = 0.6f;
    f32 restitution = 0.f;
    bool isSensor = false;
    Filter2D filter{};
    u64 userData = 0u;
};

struct RevoluteJointDef2D {
    BodyId2D bodyA = kInvalidId2D;
    BodyId2D bodyB = kInvalidId2D;
    vec2 localAnchorA{0.f, 0.f};
    vec2 localAnchorB{0.f, 0.f};
    f32 referenceAngle = 0.f;
    bool enableLimit = false;
    f32 lowerAngle = 0.f;
    f32 upperAngle = 0.f;
    bool enableMotor = false;
    f32 motorSpeed = 0.f;
    f32 maxMotorTorque = 0.f;
    bool collideConnected = false;
    /// Fills the local anchors and reference angle from a world anchor and the bodies' current pose.
    void initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchor);
};

struct DistanceJointDef2D {
    BodyId2D bodyA = kInvalidId2D;
    BodyId2D bodyB = kInvalidId2D;
    vec2 localAnchorA{0.f, 0.f};
    vec2 localAnchorB{0.f, 0.f};
    f32 length = 1.f;
    /// Spring frequency in Hz; 0 makes the joint rigid.
    f32 frequencyHz = 0.f;
    f32 dampingRatio = 0.f;
    bool collideConnected = false;
    /// Fills the local anchors and the rest length from two world anchors.
    void initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchorA,
                    vec2 worldAnchorB);
};

struct PrismaticJointDef2D {
    BodyId2D bodyA = kInvalidId2D;
    BodyId2D bodyB = kInvalidId2D;
    vec2 localAnchorA{0.f, 0.f};
    vec2 localAnchorB{0.f, 0.f};
    /// Slide axis in body A's frame (normalised on creation).
    vec2 localAxisA{1.f, 0.f};
    f32 referenceAngle = 0.f;
    bool enableLimit = false;
    f32 lowerTranslation = 0.f;
    f32 upperTranslation = 0.f;
    bool enableMotor = false;
    f32 motorSpeed = 0.f;
    f32 maxMotorForce = 0.f;
    bool collideConnected = false;
    void initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchor,
                    vec2 worldAxis);
};

/// Contact begin / end event between two non-sensor shapes (shapeA < shapeB). For begin events the
/// normal points from A to B and point is the first world contact point.
struct ContactEvent2D {
    ShapeId2D shapeA = kInvalidId2D;
    ShapeId2D shapeB = kInvalidId2D;
    BodyId2D bodyA = kInvalidId2D;
    BodyId2D bodyB = kInvalidId2D;
    vec2 normal{0.f, 0.f};
    vec2 point{0.f, 0.f};
    /// Closing speed along the normal when the contact began (0 for end events).
    f32 approachSpeed = 0.f;
};

/// A visitor shape started / stopped overlapping a sensor shape.
struct SensorEvent2D {
    ShapeId2D sensorShape = kInvalidId2D;
    ShapeId2D visitorShape = kInvalidId2D;
    BodyId2D sensorBody = kInvalidId2D;
    BodyId2D visitorBody = kInvalidId2D;
};

/// Optional listener, called at the end of step() in event order (begin events, then end events).
class ContactListener2D {
public:
    virtual ~ContactListener2D() = default;
    virtual void beginContact(const ContactEvent2D& /*event*/) {}
    virtual void endContact(const ContactEvent2D& /*event*/) {}
    virtual void beginSensor(const SensorEvent2D& /*event*/) {}
    virtual void endSensor(const SensorEvent2D& /*event*/) {}
};

struct RayHit2D {
    bool hit = false;
    ShapeId2D shape = kInvalidId2D;
    BodyId2D body = kInvalidId2D;
    vec2 point{0.f, 0.f};
    vec2 normal{0.f, 0.f};
    /// Fraction along p1 -> p2 in [0, 1].
    f32 fraction = 1.f;
};

struct Aabb2D {
    vec2 lower{0.f, 0.f};
    vec2 upper{0.f, 0.f};
};

struct SolverSettings2D {
    vec2 gravity{0.f, -10.f};
    u32 velocityIterations = 8u;
    u32 positionIterations = 3u;
    bool warmStarting = true;
};

struct RigidStepStats2D {
    u32 candidatePairs = 0u;
    u32 contacts = 0u;
    u32 touchingContacts = 0u;
    u32 sensorOverlaps = 0u;
    u32 manifoldPoints = 0u;
};

/// 2D physics facade composed by World2D — does not inherit Box2D.
///
/// Two layers live here. The legacy sprite bodies (addCircleBody / addBoxBody / set/getBodyPosition)
/// stay on the shared PhysicsPipeline (SpatialHash2D broadphase, translation-only PBD) because World2D's
/// sprite sync and its gates read that pipeline. The rigid-body API below (createBody / add*Shape /
/// create*Joint / ray and AABB queries / events) is the full 2D dynamics world; step() advances both.
/// The two layers do not collide with each other.
class PhysicsWorld2D {
public:
    PhysicsWorld2D();
    ~PhysicsWorld2D();
    PhysicsWorld2D(const PhysicsWorld2D&) = delete;
    PhysicsWorld2D& operator=(const PhysicsWorld2D&) = delete;
    PhysicsWorld2D(PhysicsWorld2D&&) noexcept;
    PhysicsWorld2D& operator=(PhysicsWorld2D&&) noexcept;

    void init(const PhysicsPipelineDesc& desc = {});
    /// Clears both layers (keeps buffer capacity) and restores default solver settings.
    void reset();

    // --- legacy pipeline bodies (World2D sprite sync) -------------------------------------------
    u32 addCircleBody(float x, float y, f32 radius, f32 invMass = 1.f, u32 collisionLayer = 1u,
                      u32 collisionMask = 0xFFFFFFFFu);
    u32 addBoxBody(float x, float y, f32 halfWidth, f32 halfHeight, f32 invMass = 1.f,
                   u32 collisionLayer = 1u, u32 collisionMask = 0xFFFFFFFFu);
    void setBodyPosition(u32 bodyIndex, float x, float y);
    void getBodyPosition(u32 bodyIndex, float& x, float& y) const;

    /// Steps the legacy pipeline and the rigid-body world by dt.
    void step(f32 dt);

    u32 bodyCount() const { return m_pipeline.bodyCount(); }
    u32 contactCount() const { return m_pipeline.contactCount(); }
    const PhysicsPipeline& pipeline() const { return m_pipeline; }

    // --- rigid-body world ------------------------------------------------------------------------
    void setSolverSettings(const SolverSettings2D& settings);
    const SolverSettings2D& solverSettings() const;
    void setGravity(vec2 gravity);
    /// Steps only the rigid-body world.
    void stepRigid(f32 dt);

    BodyId2D createBody(const BodyDef2D& def);
    /// Removes the body, its shapes and its joints; touching contacts report end events next step.
    void destroyBody(BodyId2D body);
    bool bodyAlive(BodyId2D body) const;
    u32 rigidBodyCount() const;

    ShapeId2D addCircleShape(BodyId2D body, f32 radius, vec2 localCenter = {0.f, 0.f},
                             const ShapeDef2D& def = {});
    /// Convex hull of the points (at most kMaxPolygonVertices2D, not degenerate). Returns
    /// kInvalidId2D when no valid hull exists.
    ShapeId2D addPolygonShape(BodyId2D body, std::span<const vec2> points, const ShapeDef2D& def = {});
    ShapeId2D addBoxShape(BodyId2D body, f32 halfWidth, f32 halfHeight, vec2 localCenter = {0.f, 0.f},
                          f32 localAngle = 0.f, const ShapeDef2D& def = {});
    /// Two-sided segment.
    ShapeId2D addEdgeShape(BodyId2D body, vec2 v1, vec2 v2, const ShapeDef2D& def = {});
    /// One-sided chain of edges with ghost vertices. Returns the first edge's id; the chain's edges
    /// have consecutive ids (points.size() - 1 edges, or points.size() for a loop).
    ShapeId2D addChainShape(BodyId2D body, std::span<const vec2> points, bool loop,
                            const ShapeDef2D& def = {});
    u32 shapeCount() const;
    bool shapeAlive(ShapeId2D shape) const;
    BodyId2D shapeBody(ShapeId2D shape) const;
    ShapeType2D shapeType(ShapeId2D shape) const;
    Aabb2D shapeAabb(ShapeId2D shape) const;
    Filter2D shapeFilter(ShapeId2D shape) const;
    bool shapeIsSensor(ShapeId2D shape) const;
    void setShapeFilter(ShapeId2D shape, const Filter2D& filter);
    void setShapeSensor(ShapeId2D shape, bool isSensor);

    JointId2D createRevoluteJoint(const RevoluteJointDef2D& def);
    JointId2D createDistanceJoint(const DistanceJointDef2D& def);
    JointId2D createPrismaticJoint(const PrismaticJointDef2D& def);
    void destroyJoint(JointId2D joint);
    u32 jointCount() const;
    f32 revoluteJointAngle(JointId2D joint) const;
    f32 prismaticJointTranslation(JointId2D joint) const;
    void setJointMotor(JointId2D joint, bool enable, f32 speed, f32 maxForceOrTorque);
    /// Last step's constraint impulse magnitude (point / axial impulse, for break-force checks).
    f32 jointReactionImpulse(JointId2D joint) const;

    BodyType2D bodyType(BodyId2D body) const;
    vec2 bodyPosition(BodyId2D body) const;
    f32 bodyAngle(BodyId2D body) const;
    vec2 bodyWorldCenter(BodyId2D body) const;
    vec2 bodyLinearVelocity(BodyId2D body) const;
    f32 bodyAngularVelocity(BodyId2D body) const;
    f32 bodyMass(BodyId2D body) const;
    /// Rotational inertia about the centre of mass.
    f32 bodyInertia(BodyId2D body) const;
    u64 bodyUserData(BodyId2D body) const;
    vec2 bodyWorldPoint(BodyId2D body, vec2 localPoint) const;
    vec2 bodyLocalPoint(BodyId2D body, vec2 worldPoint) const;
    void setBodyTransform(BodyId2D body, vec2 position, f32 angle);
    void setBodyLinearVelocity(BodyId2D body, vec2 velocity);
    void setBodyAngularVelocity(BodyId2D body, f32 omega);
    void setBodyType(BodyId2D body, BodyType2D type);
    void applyForce(BodyId2D body, vec2 force, vec2 worldPoint);
    void applyForceToCenter(BodyId2D body, vec2 force);
    void applyTorque(BodyId2D body, f32 torque);
    void applyLinearImpulse(BodyId2D body, vec2 impulse, vec2 worldPoint);
    void applyAngularImpulse(BodyId2D body, f32 impulse);

    // --- queries (current poses; accelerated by a BVH rebuilt lazily after changes) ---------------
    /// Closest hit along p1 -> p2 over shapes passing maskBits (tested against category bits).
    RayHit2D rayCastClosest(vec2 p1, vec2 p2, u32 maskBits = 0xFFFFFFFFu, bool includeSensors = false) const;
    /// All hits along p1 -> p2 sorted by (fraction, shape id). Returns the hit count.
    u32 rayCastAll(vec2 p1, vec2 p2, std::vector<RayHit2D>& outHits, u32 maskBits = 0xFFFFFFFFu,
                   bool includeSensors = false) const;
    /// Ray against one shape (no broadphase), for tools and reference checks.
    bool rayCastShape(ShapeId2D shape, vec2 p1, vec2 p2, RayHit2D& outHit) const;
    /// Shapes whose AABB overlaps [lower, upper], sorted by id. Returns the count.
    u32 queryAabb(const Aabb2D& box, std::vector<ShapeId2D>& outShapes, u32 maskBits = 0xFFFFFFFFu) const;
    /// Shapes that contain the point (edges never do), sorted by id.
    u32 queryPoint(vec2 point, std::vector<ShapeId2D>& outShapes, u32 maskBits = 0xFFFFFFFFu) const;

    // --- events (valid until the next step) ------------------------------------------------------
    void setContactListener(ContactListener2D* listener);
    std::span<const ContactEvent2D> contactBeginEvents() const;
    std::span<const ContactEvent2D> contactEndEvents() const;
    std::span<const SensorEvent2D> sensorBeginEvents() const;
    std::span<const SensorEvent2D> sensorEndEvents() const;
    const RigidStepStats2D& rigidStepStats() const;
    /// Number of touching shape pairs involving the body (excluding sensors).
    u32 bodyContactCount(BodyId2D body) const;

    p2d::RigidWorld2D& rigid() { return *m_rigid; }
    const p2d::RigidWorld2D& rigid() const { return *m_rigid; }

private:
    PhysicsPipeline m_pipeline;
    std::unique_ptr<p2d::RigidWorld2D> m_rigid;
};

} // namespace fuse::physics
