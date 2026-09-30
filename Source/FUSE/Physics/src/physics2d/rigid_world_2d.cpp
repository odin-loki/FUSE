#include "rigid_world_2d.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::physics::p2d {

namespace {

u64 pairKey(u32 a, u32 b) {
    const u32 lo = a < b ? a : b;
    const u32 hi = a < b ? b : a;
    return (static_cast<u64>(lo) << 32u) | static_cast<u64>(hi);
}

u32 keyLo(u64 key) { return static_cast<u32>(key >> 32u); }
u32 keyHi(u64 key) { return static_cast<u32>(key & 0xFFFFFFFFull); }

bool overlapY(const Aabb2D& a, const Aabb2D& b) { return a.lower.y <= b.upper.y && b.lower.y <= a.upper.y; }

} // namespace

void RigidWorld2D::clear() {
    m_bodies.clear();
    m_freeBodies.clear();
    m_shapes.clear();
    m_freeShapes.clear();
    m_joints.clear();
    m_freeJoints.clear();
    m_aliveBodies = 0u;
    m_aliveShapes = 0u;
    m_aliveJoints = 0u;
    m_proxies.clear();
    m_pairKeys.clear();
    m_jointPairs.clear();
    m_jointPairsDirty = false;
    m_contacts.clear();
    m_contactsScratch.clear();
    m_constraints.clear();
    m_beginEvents.clear();
    m_endEvents.clear();
    m_sensorBegin.clear();
    m_sensorEnd.clear();
    m_pendingEnd.clear();
    m_pendingSensorEnd.clear();
    m_stats = {};
    m_prevDt = 0.f;
    m_dtRatio = 1.f;
    m_bvhDirty = true;
    m_bvhNodes.clear();
    m_bvhItems.clear();
    m_queryAabbs.clear();
    m_bvhRoot = kInvalidId2D;
    settings = {};
    listener = nullptr;
}

// --- bodies -----------------------------------------------------------------------------------------

BodyId2D RigidWorld2D::createBody(const BodyDef2D& def) {
    u32 id = 0u;
    if (!m_freeBodies.empty()) {
        id = m_freeBodies.back();
        m_freeBodies.pop_back();
    } else {
        id = static_cast<u32>(m_bodies.size());
        m_bodies.emplace_back();
    }
    Body2D& b = m_bodies[id];
    b = Body2D{};
    b.alive = true;
    b.type = def.type;
    b.fixedRotation = def.fixedRotation;
    b.xf.p = def.position;
    b.xf.q = Rot(def.angle);
    b.c = def.position;
    b.a = def.angle;
    if (def.type != BodyType2D::Static) {
        b.v = def.linearVelocity;
        b.w = def.angularVelocity;
    }
    b.linearDamping = def.linearDamping;
    b.angularDamping = def.angularDamping;
    b.gravityScale = def.gravityScale;
    b.userData = def.userData;
    ++m_aliveBodies;
    resetMassData(id);
    m_bvhDirty = true;
    return id;
}

void RigidWorld2D::destroyBody(BodyId2D id) {
    if (!bodyValid(id)) {
        return;
    }
    for (u32 j = 0; j < m_joints.size(); ++j) {
        if (m_joints[j].alive && (m_joints[j].bodyA == id || m_joints[j].bodyB == id)) {
            destroyJoint(j);
        }
    }
    Body2D& b = m_bodies[id];
    for (u32 s = b.firstShape; s != kInvalidId2D;) {
        Shape2D& shape = m_shapes[s];
        const u32 next = shape.nextOnBody;
        shape.alive = false;
        m_freeShapes.push_back(s);
        --m_aliveShapes;
        s = next;
    }
    removeDeadShapeContacts();
    b.alive = false;
    b.firstShape = kInvalidId2D;
    b.lastShape = kInvalidId2D;
    m_freeBodies.push_back(id);
    --m_aliveBodies;
    m_bvhDirty = true;
}

void RigidWorld2D::removeDeadShapeContacts() {
    usize write = 0u;
    for (usize i = 0; i < m_contacts.size(); ++i) {
        const Contact2D& c = m_contacts[i];
        const bool dead = !m_shapes[c.shapeA].alive || !m_shapes[c.shapeB].alive;
        if (!dead) {
            if (write != i) {
                m_contacts[write] = c;
            }
            ++write;
            continue;
        }
        if (c.touching) {
            // Shapes of the destroyed body are marked dead but keep their data until reused.
            if (c.sensor) {
                const bool aSensor = m_shapes[c.shapeA].isSensor;
                SensorEvent2D e;
                e.sensorShape = aSensor ? c.shapeA : c.shapeB;
                e.visitorShape = aSensor ? c.shapeB : c.shapeA;
                e.sensorBody = m_shapes[e.sensorShape].body;
                e.visitorBody = m_shapes[e.visitorShape].body;
                m_pendingSensorEnd.push_back(e);
            } else {
                ContactEvent2D e;
                e.shapeA = keyLo(c.key);
                e.shapeB = keyHi(c.key);
                e.bodyA = m_shapes[e.shapeA].body;
                e.bodyB = m_shapes[e.shapeB].body;
                m_pendingEnd.push_back(e);
            }
        }
    }
    m_contacts.resize(write);
}

void RigidWorld2D::resetMassData(BodyId2D id) {
    Body2D& b = m_bodies[id];
    b.mass = 0.f;
    b.invMass = 0.f;
    b.inertia = 0.f;
    b.invI = 0.f;
    b.localCenter = {0.f, 0.f};
    if (b.type != BodyType2D::Dynamic) {
        b.c = b.xf.p;
        return;
    }
    vec2 localCenter{0.f, 0.f};
    f32 rotationalInertia = 0.f;
    for (u32 s = b.firstShape; s != kInvalidId2D; s = m_shapes[s].nextOnBody) {
        const Shape2D& shape = m_shapes[s];
        if (shape.density == 0.f) {
            continue;
        }
        const MassData md = computeMass(shape);
        b.mass += md.mass;
        localCenter += md.mass * md.center;
        rotationalInertia += md.inertia;
    }
    if (b.mass > 0.f) {
        b.invMass = 1.f / b.mass;
        localCenter = b.invMass * localCenter;
    } else {
        b.mass = 1.f;
        b.invMass = 1.f;
    }
    if (rotationalInertia > 0.f && !b.fixedRotation) {
        b.inertia = rotationalInertia - b.mass * dot(localCenter, localCenter);
        b.invI = b.inertia > 0.f ? 1.f / b.inertia : 0.f;
    } else {
        b.inertia = 0.f;
        b.invI = 0.f;
    }
    const vec2 oldCenter = b.c;
    b.localCenter = localCenter;
    b.c = mul(b.xf, localCenter);
    b.v += cross(b.w, b.c - oldCenter);
}

void RigidWorld2D::setTransform(BodyId2D id, vec2 position, f32 angle) {
    if (!bodyValid(id)) {
        return;
    }
    Body2D& b = m_bodies[id];
    b.xf.p = position;
    b.xf.q = Rot(angle);
    b.a = angle;
    b.c = mul(b.xf, b.localCenter);
    m_bvhDirty = true;
}

void RigidWorld2D::setType(BodyId2D id, BodyType2D type) {
    if (!bodyValid(id)) {
        return;
    }
    Body2D& b = m_bodies[id];
    if (b.type == type) {
        return;
    }
    b.type = type;
    if (type == BodyType2D::Static) {
        b.v = {0.f, 0.f};
        b.w = 0.f;
    }
    b.force = {0.f, 0.f};
    b.torque = 0.f;
    resetMassData(id);
}

// --- shapes -----------------------------------------------------------------------------------------

ShapeId2D RigidWorld2D::addShape(BodyId2D bodyId, const Shape2D& shape, const ShapeDef2D& def, bool reuseFreeSlot) {
    if (!bodyValid(bodyId)) {
        return kInvalidId2D;
    }
    u32 id = 0u;
    if (reuseFreeSlot && !m_freeShapes.empty()) {
        id = m_freeShapes.back();
        m_freeShapes.pop_back();
    } else {
        id = static_cast<u32>(m_shapes.size());
        m_shapes.emplace_back();
    }
    Shape2D& s = m_shapes[id];
    s = shape;
    s.alive = true;
    s.body = bodyId;
    s.isSensor = def.isSensor;
    s.density = s.type == ShapeType2D::Edge ? 0.f : std::max(def.density, 0.f);
    s.friction = std::max(def.friction, 0.f);
    s.restitution = std::max(def.restitution, 0.f);
    s.filter = def.filter;
    s.userData = def.userData;
    Body2D& b = m_bodies[bodyId];
    // Append so the body's shape list keeps creation order (deterministic mass accumulation).
    s.nextOnBody = kInvalidId2D;
    if (b.firstShape == kInvalidId2D) {
        b.firstShape = id;
    } else {
        m_shapes[b.lastShape].nextOnBody = id;
    }
    b.lastShape = id;
    ++m_aliveShapes;
    if (b.type == BodyType2D::Dynamic && s.density > 0.f) {
        resetMassData(bodyId);
    }
    m_bvhDirty = true;
    return id;
}

// --- joints -----------------------------------------------------------------------------------------

JointId2D RigidWorld2D::addJoint(const Joint2D& joint) {
    if (!bodyValid(joint.bodyA) || !bodyValid(joint.bodyB) || joint.bodyA == joint.bodyB) {
        return kInvalidId2D;
    }
    u32 id = 0u;
    if (!m_freeJoints.empty()) {
        id = m_freeJoints.back();
        m_freeJoints.pop_back();
    } else {
        id = static_cast<u32>(m_joints.size());
        m_joints.emplace_back();
    }
    m_joints[id] = joint;
    m_joints[id].alive = true;
    ++m_aliveJoints;
    m_jointPairsDirty = true;
    return id;
}

void RigidWorld2D::destroyJoint(JointId2D id) {
    if (!jointValid(id)) {
        return;
    }
    m_joints[id].alive = false;
    m_freeJoints.push_back(id);
    --m_aliveJoints;
    m_jointPairsDirty = true;
}

void RigidWorld2D::rebuildJointPairs() {
    m_jointPairs.clear();
    for (const Joint2D& j : m_joints) {
        if (j.alive && !j.collideConnected) {
            m_jointPairs.push_back(pairKey(j.bodyA, j.bodyB));
        }
    }
    std::sort(m_jointPairs.begin(), m_jointPairs.end());
    m_jointPairsDirty = false;
}

// --- broadphase / contacts --------------------------------------------------------------------------

bool RigidWorld2D::shouldCollide(u32 ia, u32 ib) const {
    const Shape2D& a = m_shapes[ia];
    const Shape2D& b = m_shapes[ib];
    if (a.body == b.body) {
        return false;
    }
    if (a.isSensor && b.isSensor) {
        return false;
    }
    if (!canCollide(a.type, b.type)) {
        return false;
    }
    const Body2D& ba = m_bodies[a.body];
    const Body2D& bb = m_bodies[b.body];
    if (ba.type != BodyType2D::Dynamic && bb.type != BodyType2D::Dynamic) {
        return false;
    }
    if (a.filter.groupIndex == b.filter.groupIndex && a.filter.groupIndex != 0) {
        return a.filter.groupIndex > 0;
    }
    if ((a.filter.maskBits & b.filter.categoryBits) == 0u || (a.filter.categoryBits & b.filter.maskBits) == 0u) {
        return false;
    }
    if (!m_jointPairs.empty() && std::binary_search(m_jointPairs.begin(), m_jointPairs.end(), pairKey(a.body, b.body))) {
        return false;
    }
    return true;
}

void RigidWorld2D::updateAabbs() {
    for (Shape2D& s : m_shapes) {
        if (!s.alive) {
            continue;
        }
        s.aabb = computeAabb(s, m_bodies[s.body].xf);
        s.fatAabb.lower = {s.aabb.lower.x - kAabbMargin, s.aabb.lower.y - kAabbMargin};
        s.fatAabb.upper = {s.aabb.upper.x + kAabbMargin, s.aabb.upper.y + kAabbMargin};
    }
}

void RigidWorld2D::findPairs() {
    m_proxies.clear();
    for (u32 i = 0; i < m_shapes.size(); ++i) {
        const Shape2D& s = m_shapes[i];
        if (s.alive) {
            m_proxies.push_back({s.fatAabb.lower.x, s.fatAabb.upper.x, i});
        }
    }
    std::sort(m_proxies.begin(), m_proxies.end(), [](const Proxy& l, const Proxy& r) {
        return l.minX < r.minX || (l.minX == r.minX && l.shape < r.shape);
    });
    m_pairKeys.clear();
    for (usize i = 0; i < m_proxies.size(); ++i) {
        const Proxy& pi = m_proxies[i];
        const Shape2D& si = m_shapes[pi.shape];
        const bool iStatic = m_bodies[si.body].type != BodyType2D::Dynamic;
        for (usize j = i + 1u; j < m_proxies.size() && m_proxies[j].minX <= pi.maxX; ++j) {
            const Proxy& pj = m_proxies[j];
            const Shape2D& sj = m_shapes[pj.shape];
            if (iStatic && m_bodies[sj.body].type != BodyType2D::Dynamic) {
                continue;
            }
            if (!overlapY(si.fatAabb, sj.fatAabb) || !shouldCollide(pi.shape, pj.shape)) {
                continue;
            }
            m_pairKeys.push_back(pairKey(pi.shape, pj.shape));
        }
    }
    std::sort(m_pairKeys.begin(), m_pairKeys.end());
    m_stats.candidatePairs = static_cast<u32>(m_pairKeys.size());
}

ContactEvent2D RigidWorld2D::makeEvent(const Contact2D& contact) const {
    ContactEvent2D e;
    e.shapeA = keyLo(contact.key);
    e.shapeB = keyHi(contact.key);
    e.bodyA = m_shapes[e.shapeA].body;
    e.bodyB = m_shapes[e.shapeB].body;
    if (contact.manifold.pointCount == 0u) {
        return e;
    }
    const Shape2D& sa = m_shapes[contact.shapeA];
    const Shape2D& sb = m_shapes[contact.shapeB];
    const Body2D& ba = m_bodies[sa.body];
    const Body2D& bb = m_bodies[sb.body];
    WorldManifold wm;
    computeWorldManifold(contact.manifold, ba.xf, sa.radius, bb.xf, sb.radius, wm);
    const vec2 rA = wm.points[0] - ba.c;
    const vec2 rB = wm.points[0] - bb.c;
    const vec2 dv = bb.v + cross(bb.w, rB) - ba.v - cross(ba.w, rA);
    e.approachSpeed = std::max(-dot(dv, wm.normal), 0.f);
    e.point = wm.points[0];
    e.normal = contact.shapeA == e.shapeA ? wm.normal : -wm.normal;
    return e;
}

void RigidWorld2D::emitEnd(const Contact2D& contact) {
    if (!contact.touching) {
        return;
    }
    if (contact.sensor) {
        const bool aSensor = m_shapes[contact.shapeA].isSensor;
        SensorEvent2D e;
        e.sensorShape = aSensor ? contact.shapeA : contact.shapeB;
        e.visitorShape = aSensor ? contact.shapeB : contact.shapeA;
        e.sensorBody = m_shapes[e.sensorShape].body;
        e.visitorBody = m_shapes[e.visitorShape].body;
        m_sensorEnd.push_back(e);
    } else {
        ContactEvent2D e;
        e.shapeA = keyLo(contact.key);
        e.shapeB = keyHi(contact.key);
        e.bodyA = m_shapes[e.shapeA].body;
        e.bodyB = m_shapes[e.shapeB].body;
        m_endEvents.push_back(e);
    }
}

void RigidWorld2D::evaluateContact(Contact2D& c, bool isNew) {
    const Shape2D& sa = m_shapes[c.shapeA];
    const Shape2D& sb = m_shapes[c.shapeB];
    const bool sensorNow = sa.isSensor || sb.isSensor;
    if (!isNew && sensorNow != c.sensor && c.touching) {
        emitEnd(c);
        c.touching = false;
        c.manifold.pointCount = 0u;
    }
    c.sensor = sensorNow;
    c.friction = std::sqrt(sa.friction * sb.friction);
    c.restitution = std::max(sa.restitution, sb.restitution);
    const Xf& xfA = m_bodies[sa.body].xf;
    const Xf& xfB = m_bodies[sb.body].xf;
    const bool wasTouching = c.touching;
    if (c.sensor) {
        Manifold probe;
        collide(sa, xfA, sb, xfB, probe);
        c.touching = probe.pointCount > 0u;
        c.manifold.pointCount = 0u;
        if (c.touching && !wasTouching) {
            SensorEvent2D e;
            e.sensorShape = sa.isSensor ? c.shapeA : c.shapeB;
            e.visitorShape = sa.isSensor ? c.shapeB : c.shapeA;
            e.sensorBody = m_shapes[e.sensorShape].body;
            e.visitorBody = m_shapes[e.visitorShape].body;
            m_sensorBegin.push_back(e);
        } else if (!c.touching && wasTouching) {
            c.touching = true; // emitEnd reads the old state
            emitEnd(c);
            c.touching = false;
        }
        if (c.touching) {
            ++m_stats.sensorOverlaps;
        }
        return;
    }
    const Manifold old = c.manifold;
    collide(sa, xfA, sb, xfB, c.manifold);
    for (u32 i = 0; i < c.manifold.pointCount; ++i) {
        ManifoldPoint& mp = c.manifold.points[i];
        mp.normalImpulse = 0.f;
        mp.tangentImpulse = 0.f;
        for (u32 j = 0; j < old.pointCount; ++j) {
            if (old.points[j].id == mp.id) {
                mp.normalImpulse = old.points[j].normalImpulse;
                mp.tangentImpulse = old.points[j].tangentImpulse;
                break;
            }
        }
    }
    c.touching = c.manifold.pointCount > 0u;
    if (c.touching && !wasTouching) {
        m_beginEvents.push_back(makeEvent(c));
    } else if (!c.touching && wasTouching) {
        c.touching = true;
        emitEnd(c);
        c.touching = false;
    }
    if (c.touching) {
        ++m_stats.touchingContacts;
        m_stats.manifoldPoints += c.manifold.pointCount;
    }
}

void RigidWorld2D::updateContacts() {
    m_contactsScratch.clear();
    usize i = 0u;
    usize j = 0u;
    while (i < m_contacts.size() || j < m_pairKeys.size()) {
        if (j == m_pairKeys.size() || (i < m_contacts.size() && m_contacts[i].key < m_pairKeys[j])) {
            emitEnd(m_contacts[i]); // pair left the broadphase
            ++i;
        } else if (i == m_contacts.size() || m_pairKeys[j] < m_contacts[i].key) {
            Contact2D c;
            c.key = m_pairKeys[j];
            u32 a = keyLo(c.key);
            u32 b = keyHi(c.key);
            if (collideOrderSwapped(m_shapes[a].type, m_shapes[b].type)) {
                std::swap(a, b);
            }
            c.shapeA = a;
            c.shapeB = b;
            c.sensor = m_shapes[a].isSensor || m_shapes[b].isSensor;
            c.touching = false;
            c.manifold.pointCount = 0u;
            m_contactsScratch.push_back(c);
            ++j;
        } else {
            m_contactsScratch.push_back(m_contacts[i]);
            ++i;
            ++j;
        }
    }
    m_contacts.swap(m_contactsScratch);
    m_stats.touchingContacts = 0u;
    m_stats.sensorOverlaps = 0u;
    m_stats.manifoldPoints = 0u;
    for (Contact2D& c : m_contacts) {
        evaluateContact(c, false);
    }
    m_stats.contacts = static_cast<u32>(m_contacts.size());
}

u32 RigidWorld2D::bodyContactCount(BodyId2D id) const {
    u32 count = 0u;
    for (const Contact2D& c : m_contacts) {
        if (c.touching && !c.sensor && (m_shapes[c.shapeA].body == id || m_shapes[c.shapeB].body == id)) {
            ++count;
        }
    }
    return count;
}

// --- step -------------------------------------------------------------------------------------------

void RigidWorld2D::step(f32 dt) {
    m_beginEvents.clear();
    m_endEvents.clear();
    m_sensorBegin.clear();
    m_sensorEnd.clear();
    m_endEvents.insert(m_endEvents.end(), m_pendingEnd.begin(), m_pendingEnd.end());
    m_sensorEnd.insert(m_sensorEnd.end(), m_pendingSensorEnd.begin(), m_pendingSensorEnd.end());
    m_pendingEnd.clear();
    m_pendingSensorEnd.clear();
    if (m_jointPairsDirty) {
        rebuildJointPairs();
    }
    if (dt > 0.f) {
        updateAabbs();
        findPairs();
        updateContacts();
        m_dtRatio = m_prevDt > 0.f ? dt / m_prevDt : 1.f;
        solve(dt);
        m_prevDt = dt;
        m_bvhDirty = true;
    }
    if (listener != nullptr) {
        for (const ContactEvent2D& e : m_beginEvents) {
            listener->beginContact(e);
        }
        for (const ContactEvent2D& e : m_endEvents) {
            listener->endContact(e);
        }
        for (const SensorEvent2D& e : m_sensorBegin) {
            listener->beginSensor(e);
        }
        for (const SensorEvent2D& e : m_sensorEnd) {
            listener->endSensor(e);
        }
    }
}

void RigidWorld2D::solve(f32 dt) {
    const f32 h = dt;
    for (Body2D& b : m_bodies) {
        if (!b.alive || b.type != BodyType2D::Dynamic) {
            continue;
        }
        const vec2 accel = b.gravityScale * settings.gravity + b.invMass * b.force;
        b.v += h * accel;
        b.w += h * b.invI * b.torque;
        b.v = (1.f / (1.f + h * b.linearDamping)) * b.v;
        b.w *= 1.f / (1.f + h * b.angularDamping);
    }

    initContactConstraints(m_dtRatio);
    warmStartContacts();
    for (Joint2D& j : m_joints) {
        if (j.alive) {
            initJointVelocity(j, h);
        }
    }
    for (u32 it = 0; it < settings.velocityIterations; ++it) {
        for (Joint2D& j : m_joints) {
            if (j.alive) {
                solveJointVelocity(j, h);
            }
        }
        solveContactVelocities();
    }
    storeContactImpulses();

    for (Body2D& b : m_bodies) {
        if (!b.alive || b.type == BodyType2D::Static) {
            continue;
        }
        vec2 translation = h * b.v;
        if (dot(translation, translation) > kMaxTranslation * kMaxTranslation) {
            b.v = (kMaxTranslation / length(translation)) * b.v;
            translation = h * b.v;
        }
        f32 rotation = h * b.w;
        if (rotation * rotation > kMaxRotation * kMaxRotation) {
            b.w *= kMaxRotation / std::fabs(rotation);
            rotation = h * b.w;
        }
        b.c += translation;
        b.a += rotation;
    }

    for (u32 it = 0; it < settings.positionIterations; ++it) {
        const bool contactsOk = solveContactPositions();
        bool jointsOk = true;
        for (Joint2D& j : m_joints) {
            if (j.alive) {
                jointsOk = solveJointPosition(j) && jointsOk;
            }
        }
        if (contactsOk && jointsOk) {
            break;
        }
    }

    for (Body2D& b : m_bodies) {
        if (!b.alive) {
            continue;
        }
        b.xf.q = Rot(b.a);
        b.xf.p = b.c - mul(b.xf.q, b.localCenter);
        b.force = {0.f, 0.f};
        b.torque = 0.f;
    }
}

} // namespace fuse::physics::p2d
