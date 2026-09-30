#include <fuse/physics/physics_world_2d.hpp>

#include "physics2d/rigid_world_2d.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::physics {

using p2d::RigidWorld2D;

PhysicsWorld2D::PhysicsWorld2D() : m_rigid(std::make_unique<RigidWorld2D>()) {}
PhysicsWorld2D::~PhysicsWorld2D() = default;
PhysicsWorld2D::PhysicsWorld2D(PhysicsWorld2D&&) noexcept = default;
PhysicsWorld2D& PhysicsWorld2D::operator=(PhysicsWorld2D&&) noexcept = default;

void PhysicsWorld2D::init(const PhysicsPipelineDesc& desc) {
    PhysicsPipelineDesc localDesc = desc;
    localDesc.broadphaseMode = BroadphaseMode::SpatialHash2D;
    m_pipeline.init(localDesc);
    if (!m_rigid) {
        m_rigid = std::make_unique<RigidWorld2D>();
    }
}

void PhysicsWorld2D::reset() {
    m_pipeline.reset();
    if (m_rigid) {
        m_rigid->clear();
    }
}

u32 PhysicsWorld2D::addCircleBody(float x, float y, f32 radius, f32 invMass, u32 collisionLayer,
                                  u32 collisionMask) {
    return m_pipeline.addSphereBody({x, y, 0.f}, radius, invMass, 0u, collisionLayer, collisionMask);
}

u32 PhysicsWorld2D::addBoxBody(float x, float y, f32 halfWidth, f32 halfHeight, f32 invMass,
                               u32 collisionLayer, u32 collisionMask) {
    return m_pipeline.addBoxBody({x, y, 0.f}, {halfWidth, halfHeight, 0.f}, invMass, 0u,
                                 collisionLayer, collisionMask);
}

void PhysicsWorld2D::setBodyPosition(u32 bodyIndex, float x, float y) {
    if (bodyIndex >= m_pipeline.bodies().count()) {
        return;
    }
    m_pipeline.bodies().positions.at(bodyIndex) = {x, y, 0.f};
}

void PhysicsWorld2D::getBodyPosition(u32 bodyIndex, float& x, float& y) const {
    if (bodyIndex >= m_pipeline.bodies().count()) {
        x = 0.f;
        y = 0.f;
        return;
    }
    const vec3& position = m_pipeline.bodies().positions.at(bodyIndex);
    x = position.x;
    y = position.y;
}

void PhysicsWorld2D::step(f32 dt) {
    m_pipeline.step(dt);
    m_rigid->step(dt);
}

// --- rigid-body world ---------------------------------------------------------------------------------

void PhysicsWorld2D::setSolverSettings(const SolverSettings2D& settings) { m_rigid->settings = settings; }
const SolverSettings2D& PhysicsWorld2D::solverSettings() const { return m_rigid->settings; }
void PhysicsWorld2D::setGravity(vec2 gravity) { m_rigid->settings.gravity = gravity; }
void PhysicsWorld2D::stepRigid(f32 dt) { m_rigid->step(dt); }

BodyId2D PhysicsWorld2D::createBody(const BodyDef2D& def) { return m_rigid->createBody(def); }
void PhysicsWorld2D::destroyBody(BodyId2D body) { m_rigid->destroyBody(body); }
bool PhysicsWorld2D::bodyAlive(BodyId2D body) const { return m_rigid->bodyValid(body); }
u32 PhysicsWorld2D::rigidBodyCount() const { return m_rigid->aliveBodyCount(); }

ShapeId2D PhysicsWorld2D::addCircleShape(BodyId2D body, f32 radius, vec2 localCenter, const ShapeDef2D& def) {
    if (!(radius > 0.f)) {
        return kInvalidId2D;
    }
    p2d::Shape2D shape;
    shape.type = ShapeType2D::Circle;
    shape.radius = radius;
    shape.center = localCenter;
    return m_rigid->addShape(body, shape, def);
}

ShapeId2D PhysicsWorld2D::addPolygonShape(BodyId2D body, std::span<const vec2> points, const ShapeDef2D& def) {
    p2d::Shape2D shape;
    if (!p2d::buildPolygon(shape, points.data(), static_cast<u32>(points.size()))) {
        return kInvalidId2D;
    }
    return m_rigid->addShape(body, shape, def);
}

ShapeId2D PhysicsWorld2D::addBoxShape(BodyId2D body, f32 halfWidth, f32 halfHeight, vec2 localCenter, f32 localAngle,
                                      const ShapeDef2D& def) {
    const p2d::Rot q(localAngle);
    const vec2 corners[4] = {
        localCenter + p2d::mul(q, vec2{-halfWidth, -halfHeight}),
        localCenter + p2d::mul(q, vec2{halfWidth, -halfHeight}),
        localCenter + p2d::mul(q, vec2{halfWidth, halfHeight}),
        localCenter + p2d::mul(q, vec2{-halfWidth, halfHeight}),
    };
    return addPolygonShape(body, std::span<const vec2>(corners, 4u), def);
}

ShapeId2D PhysicsWorld2D::addEdgeShape(BodyId2D body, vec2 v1, vec2 v2, const ShapeDef2D& def) {
    if (p2d::distanceSq(v1, v2) <= p2d::kLinearSlop * p2d::kLinearSlop) {
        return kInvalidId2D;
    }
    p2d::Shape2D shape;
    p2d::buildEdge(shape, v1, v2);
    shape.oneSided = false;
    return m_rigid->addShape(body, shape, def);
}

ShapeId2D PhysicsWorld2D::addChainShape(BodyId2D body, std::span<const vec2> points, bool loop, const ShapeDef2D& def) {
    const usize n = points.size();
    if (!m_rigid->bodyValid(body) || n < 2u || (loop && n < 3u)) {
        return kInvalidId2D;
    }
    for (usize i = 1; i < n; ++i) {
        if (p2d::distanceSq(points[i - 1u], points[i]) <= p2d::kLinearSlop * p2d::kLinearSlop) {
            return kInvalidId2D;
        }
    }
    const usize edgeCount = loop ? n : n - 1u;
    ShapeId2D first = kInvalidId2D;
    for (usize i = 0; i < edgeCount; ++i) {
        const vec2 v1 = points[i];
        const vec2 v2 = points[(i + 1u) % n];
        // Ghost vertices: the neighbours along the chain; an open chain's ends extrapolate straight.
        vec2 v0 = v1 + (v1 - v2);
        vec2 v3 = v2 + (v2 - v1);
        if (i > 0u) {
            v0 = points[i - 1u];
        } else if (loop) {
            v0 = points[n - 1u];
        }
        if (i + 2u < n) {
            v3 = points[i + 2u];
        } else if (loop) {
            v3 = points[(i + 2u) % n];
        }
        p2d::Shape2D shape;
        p2d::buildEdge(shape, v1, v2);
        shape.oneSided = true;
        shape.vertex0 = v0;
        shape.vertex3 = v3;
        // Chain edges bypass the free list so their ids stay consecutive.
        const ShapeId2D id = m_rigid->addShape(body, shape, def, false);
        if (first == kInvalidId2D) {
            first = id;
        }
    }
    return first;
}

u32 PhysicsWorld2D::shapeCount() const { return m_rigid->aliveShapeCount(); }
bool PhysicsWorld2D::shapeAlive(ShapeId2D shape) const { return m_rigid->shapeValid(shape); }
BodyId2D PhysicsWorld2D::shapeBody(ShapeId2D shape) const {
    return m_rigid->shapeValid(shape) ? m_rigid->shape(shape).body : kInvalidId2D;
}
ShapeType2D PhysicsWorld2D::shapeType(ShapeId2D shape) const {
    return m_rigid->shapeValid(shape) ? m_rigid->shape(shape).type : ShapeType2D::Circle;
}
Aabb2D PhysicsWorld2D::shapeAabb(ShapeId2D shape) const {
    if (!m_rigid->shapeValid(shape)) {
        return {};
    }
    const p2d::Shape2D& s = m_rigid->shape(shape);
    return p2d::computeAabb(s, m_rigid->body(s.body).xf);
}
Filter2D PhysicsWorld2D::shapeFilter(ShapeId2D shape) const {
    return m_rigid->shapeValid(shape) ? m_rigid->shape(shape).filter : Filter2D{};
}
bool PhysicsWorld2D::shapeIsSensor(ShapeId2D shape) const {
    return m_rigid->shapeValid(shape) && m_rigid->shape(shape).isSensor;
}
void PhysicsWorld2D::setShapeFilter(ShapeId2D shape, const Filter2D& filter) {
    if (m_rigid->shapeValid(shape)) {
        m_rigid->shape(shape).filter = filter;
    }
}
void PhysicsWorld2D::setShapeSensor(ShapeId2D shape, bool isSensor) {
    if (m_rigid->shapeValid(shape)) {
        m_rigid->shape(shape).isSensor = isSensor;
    }
}

// --- joints -------------------------------------------------------------------------------------------

void RevoluteJointDef2D::initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchor) {
    bodyA = a;
    bodyB = b;
    localAnchorA = world.bodyLocalPoint(a, worldAnchor);
    localAnchorB = world.bodyLocalPoint(b, worldAnchor);
    referenceAngle = world.bodyAngle(b) - world.bodyAngle(a);
}

void DistanceJointDef2D::initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchorA,
                                    vec2 worldAnchorB) {
    bodyA = a;
    bodyB = b;
    localAnchorA = world.bodyLocalPoint(a, worldAnchorA);
    localAnchorB = world.bodyLocalPoint(b, worldAnchorB);
    length = std::max(p2d::length(worldAnchorB - worldAnchorA), p2d::kLinearSlop);
}

void PrismaticJointDef2D::initialize(const PhysicsWorld2D& world, BodyId2D a, BodyId2D b, vec2 worldAnchor,
                                     vec2 worldAxis) {
    bodyA = a;
    bodyB = b;
    localAnchorA = world.bodyLocalPoint(a, worldAnchor);
    localAnchorB = world.bodyLocalPoint(b, worldAnchor);
    localAxisA = p2d::mulT(p2d::Rot(world.bodyAngle(a)), p2d::normalized(worldAxis));
    referenceAngle = world.bodyAngle(b) - world.bodyAngle(a);
}

JointId2D PhysicsWorld2D::createRevoluteJoint(const RevoluteJointDef2D& def) {
    p2d::Joint2D j;
    j.type = JointType2D::Revolute;
    j.bodyA = def.bodyA;
    j.bodyB = def.bodyB;
    j.localAnchorA = def.localAnchorA;
    j.localAnchorB = def.localAnchorB;
    j.referenceAngle = def.referenceAngle;
    j.enableLimit = def.enableLimit;
    j.lower = std::min(def.lowerAngle, def.upperAngle);
    j.upper = std::max(def.lowerAngle, def.upperAngle);
    j.enableMotor = def.enableMotor;
    j.motorSpeed = def.motorSpeed;
    j.maxMotor = def.maxMotorTorque;
    j.collideConnected = def.collideConnected;
    return m_rigid->addJoint(j);
}

JointId2D PhysicsWorld2D::createDistanceJoint(const DistanceJointDef2D& def) {
    p2d::Joint2D j;
    j.type = JointType2D::Distance;
    j.bodyA = def.bodyA;
    j.bodyB = def.bodyB;
    j.localAnchorA = def.localAnchorA;
    j.localAnchorB = def.localAnchorB;
    j.length = std::max(def.length, p2d::kLinearSlop);
    j.frequencyHz = std::max(def.frequencyHz, 0.f);
    j.dampingRatio = std::max(def.dampingRatio, 0.f);
    j.collideConnected = def.collideConnected;
    return m_rigid->addJoint(j);
}

JointId2D PhysicsWorld2D::createPrismaticJoint(const PrismaticJointDef2D& def) {
    p2d::Joint2D j;
    j.type = JointType2D::Prismatic;
    j.bodyA = def.bodyA;
    j.bodyB = def.bodyB;
    j.localAnchorA = def.localAnchorA;
    j.localAnchorB = def.localAnchorB;
    vec2 axis = def.localAxisA;
    if (p2d::normalize(axis) == 0.f) {
        axis = {1.f, 0.f};
    }
    j.localXAxisA = axis;
    j.localYAxisA = p2d::cross(1.f, axis);
    j.referenceAngle = def.referenceAngle;
    j.enableLimit = def.enableLimit;
    j.lower = std::min(def.lowerTranslation, def.upperTranslation);
    j.upper = std::max(def.lowerTranslation, def.upperTranslation);
    j.enableMotor = def.enableMotor;
    j.motorSpeed = def.motorSpeed;
    j.maxMotor = def.maxMotorForce;
    j.collideConnected = def.collideConnected;
    return m_rigid->addJoint(j);
}

void PhysicsWorld2D::destroyJoint(JointId2D joint) { m_rigid->destroyJoint(joint); }
u32 PhysicsWorld2D::jointCount() const { return m_rigid->aliveJointCount(); }

f32 PhysicsWorld2D::revoluteJointAngle(JointId2D joint) const {
    if (!m_rigid->jointValid(joint)) {
        return 0.f;
    }
    const p2d::Joint2D& j = m_rigid->joint(joint);
    return m_rigid->body(j.bodyB).a - m_rigid->body(j.bodyA).a - j.referenceAngle;
}

f32 PhysicsWorld2D::prismaticJointTranslation(JointId2D joint) const {
    if (!m_rigid->jointValid(joint)) {
        return 0.f;
    }
    const p2d::Joint2D& j = m_rigid->joint(joint);
    const p2d::Body2D& a = m_rigid->body(j.bodyA);
    const p2d::Body2D& b = m_rigid->body(j.bodyB);
    const vec2 pA = p2d::mul(a.xf, j.localAnchorA);
    const vec2 pB = p2d::mul(b.xf, j.localAnchorB);
    return p2d::dot(pB - pA, p2d::mul(a.xf.q, j.localXAxisA));
}

void PhysicsWorld2D::setJointMotor(JointId2D joint, bool enable, f32 speed, f32 maxForceOrTorque) {
    if (!m_rigid->jointValid(joint)) {
        return;
    }
    p2d::Joint2D& j = m_rigid->joint(joint);
    j.enableMotor = enable;
    j.motorSpeed = speed;
    j.maxMotor = maxForceOrTorque;
}

f32 PhysicsWorld2D::jointReactionImpulse(JointId2D joint) const {
    return m_rigid->jointValid(joint) ? m_rigid->joint(joint).lastImpulse : 0.f;
}

// --- body state ---------------------------------------------------------------------------------------

BodyType2D PhysicsWorld2D::bodyType(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).type : BodyType2D::Static;
}
vec2 PhysicsWorld2D::bodyPosition(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).xf.p : vec2{0.f, 0.f};
}
f32 PhysicsWorld2D::bodyAngle(BodyId2D body) const { return m_rigid->bodyValid(body) ? m_rigid->body(body).a : 0.f; }
vec2 PhysicsWorld2D::bodyWorldCenter(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).c : vec2{0.f, 0.f};
}
vec2 PhysicsWorld2D::bodyLinearVelocity(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).v : vec2{0.f, 0.f};
}
f32 PhysicsWorld2D::bodyAngularVelocity(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).w : 0.f;
}
f32 PhysicsWorld2D::bodyMass(BodyId2D body) const {
    return m_rigid->bodyValid(body) && m_rigid->body(body).type == BodyType2D::Dynamic ? m_rigid->body(body).mass : 0.f;
}
f32 PhysicsWorld2D::bodyInertia(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).inertia : 0.f;
}
u64 PhysicsWorld2D::bodyUserData(BodyId2D body) const {
    return m_rigid->bodyValid(body) ? m_rigid->body(body).userData : 0u;
}
vec2 PhysicsWorld2D::bodyWorldPoint(BodyId2D body, vec2 localPoint) const {
    return m_rigid->bodyValid(body) ? p2d::mul(m_rigid->body(body).xf, localPoint) : localPoint;
}
vec2 PhysicsWorld2D::bodyLocalPoint(BodyId2D body, vec2 worldPoint) const {
    return m_rigid->bodyValid(body) ? p2d::mulT(m_rigid->body(body).xf, worldPoint) : worldPoint;
}
void PhysicsWorld2D::setBodyTransform(BodyId2D body, vec2 position, f32 angle) {
    m_rigid->setTransform(body, position, angle);
}
void PhysicsWorld2D::setBodyLinearVelocity(BodyId2D body, vec2 velocity) {
    if (m_rigid->bodyValid(body) && m_rigid->body(body).type != BodyType2D::Static) {
        m_rigid->body(body).v = velocity;
    }
}
void PhysicsWorld2D::setBodyAngularVelocity(BodyId2D body, f32 omega) {
    if (m_rigid->bodyValid(body) && m_rigid->body(body).type != BodyType2D::Static) {
        m_rigid->body(body).w = omega;
    }
}
void PhysicsWorld2D::setBodyType(BodyId2D body, BodyType2D type) { m_rigid->setType(body, type); }

void PhysicsWorld2D::applyForce(BodyId2D body, vec2 force, vec2 worldPoint) {
    if (!m_rigid->bodyValid(body) || m_rigid->body(body).type != BodyType2D::Dynamic) {
        return;
    }
    p2d::Body2D& b = m_rigid->body(body);
    b.force = b.force + force;
    b.torque += p2d::cross(worldPoint - b.c, force);
}
void PhysicsWorld2D::applyForceToCenter(BodyId2D body, vec2 force) {
    if (m_rigid->bodyValid(body) && m_rigid->body(body).type == BodyType2D::Dynamic) {
        p2d::Body2D& b = m_rigid->body(body);
        b.force = b.force + force;
    }
}
void PhysicsWorld2D::applyTorque(BodyId2D body, f32 torque) {
    if (m_rigid->bodyValid(body) && m_rigid->body(body).type == BodyType2D::Dynamic) {
        m_rigid->body(body).torque += torque;
    }
}
void PhysicsWorld2D::applyLinearImpulse(BodyId2D body, vec2 impulse, vec2 worldPoint) {
    if (!m_rigid->bodyValid(body) || m_rigid->body(body).type != BodyType2D::Dynamic) {
        return;
    }
    p2d::Body2D& b = m_rigid->body(body);
    b.v = b.v + impulse * b.invMass;
    b.w += b.invI * p2d::cross(worldPoint - b.c, impulse);
}
void PhysicsWorld2D::applyAngularImpulse(BodyId2D body, f32 impulse) {
    if (m_rigid->bodyValid(body) && m_rigid->body(body).type == BodyType2D::Dynamic) {
        p2d::Body2D& b = m_rigid->body(body);
        b.w += b.invI * impulse;
    }
}

// --- queries / events ---------------------------------------------------------------------------------

RayHit2D PhysicsWorld2D::rayCastClosest(vec2 p1, vec2 p2, u32 maskBits, bool includeSensors) const {
    return m_rigid->rayCastClosest(p1, p2, maskBits, includeSensors);
}
u32 PhysicsWorld2D::rayCastAll(vec2 p1, vec2 p2, std::vector<RayHit2D>& outHits, u32 maskBits,
                               bool includeSensors) const {
    return m_rigid->rayCastAll(p1, p2, outHits, maskBits, includeSensors);
}
bool PhysicsWorld2D::rayCastShape(ShapeId2D shape, vec2 p1, vec2 p2, RayHit2D& outHit) const {
    return m_rigid->rayCastShape(shape, p1, p2, 1.f, outHit);
}
u32 PhysicsWorld2D::queryAabb(const Aabb2D& box, std::vector<ShapeId2D>& outShapes, u32 maskBits) const {
    return m_rigid->queryAabb(box, outShapes, maskBits);
}
u32 PhysicsWorld2D::queryPoint(vec2 point, std::vector<ShapeId2D>& outShapes, u32 maskBits) const {
    return m_rigid->queryPoint(point, outShapes, maskBits);
}

void PhysicsWorld2D::setContactListener(ContactListener2D* listener) { m_rigid->listener = listener; }
std::span<const ContactEvent2D> PhysicsWorld2D::contactBeginEvents() const { return m_rigid->beginEvents(); }
std::span<const ContactEvent2D> PhysicsWorld2D::contactEndEvents() const { return m_rigid->endEvents(); }
std::span<const SensorEvent2D> PhysicsWorld2D::sensorBeginEvents() const { return m_rigid->sensorBeginEvents(); }
std::span<const SensorEvent2D> PhysicsWorld2D::sensorEndEvents() const { return m_rigid->sensorEndEvents(); }
const RigidStepStats2D& PhysicsWorld2D::rigidStepStats() const { return m_rigid->stats(); }
u32 PhysicsWorld2D::bodyContactCount(BodyId2D body) const { return m_rigid->bodyContactCount(body); }

} // namespace fuse::physics
