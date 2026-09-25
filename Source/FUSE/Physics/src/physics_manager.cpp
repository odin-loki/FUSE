#include <fuse/physics/physics_manager.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::physics {

namespace {

constexpr u32 kNoBody = 0xFFFFFFFFu;

vec3 toPhysics(const fuse::ecs::vec3& v) {
    return {v.x, v.y, v.z};
}

fuse::ecs::vec3 toEcs(vec3 v, f32 w) {
    return {v.x, v.y, v.z, w};
}

quat toPhysics(const fuse::ecs::quat& q) {
    return {q.x, q.y, q.z, q.w};
}

fuse::ecs::quat toEcs(const quat& q) {
    return {q.x, q.y, q.z, q.w};
}

bool sameVec(vec3 a, vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool sameQuat(const quat& a, const quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

u64 pairKey(fuse::ecs::EntityID a, fuse::ecs::EntityID b) {
    const u32 lo = std::min(a.index, b.index);
    const u32 hi = std::max(a.index, b.index);
    return (static_cast<u64>(lo) << 32u) | hi;
}

void wake(RigidBodySoA& bodies, u32 body) {
    bodies.flags[body] &= ~RB_SLEEPING;
    bodies.sleepTimers[body] = 0.f;
}

/// Ray vs sphere; t of the first hit at or after 0.
bool raySphere(vec3 origin, vec3 dir, vec3 center, f32 radius, f32& t) {
    const vec3 m = origin - center;
    const f32 b = m.dot(dir);
    const f32 c = m.dot(m) - radius * radius;
    if (c > 0.f && b > 0.f) {
        return false;
    }
    const f32 disc = b * b - c;
    if (disc < 0.f) {
        return false;
    }
    t = std::max(0.f, -b - std::sqrt(disc));
    return true;
}

bool rayAabb(vec3 origin, vec3 dir, vec3 lo, vec3 hi, f32& t, vec3& normal) {
    f32 tMin = 0.f;
    f32 tMax = std::numeric_limits<f32>::max();
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {dir.x, dir.y, dir.z};
    const f32 mn[3] = {lo.x, lo.y, lo.z};
    const f32 mx[3] = {hi.x, hi.y, hi.z};
    int axis = -1;
    f32 sign = 0.f;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-12f) {
            if (o[a] < mn[a] || o[a] > mx[a]) {
                return false;
            }
            continue;
        }
        f32 t0 = (mn[a] - o[a]) / d[a];
        f32 t1 = (mx[a] - o[a]) / d[a];
        f32 s = -1.f;
        if (t0 > t1) {
            std::swap(t0, t1);
            s = 1.f;
        }
        if (t0 > tMin) {
            tMin = t0;
            axis = a;
            sign = s;
        }
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    t = tMin;
    normal = {axis == 0 ? sign : 0.f, axis == 1 ? sign : 0.f, axis == 2 ? sign : 0.f};
    return true;
}

f32 capsuleDistance(vec3 p, vec3 center, vec3 params, const quat& orientation) {
    const vec3 half = capsuleHalfAxis(orientation, params.y);
    const vec3 a = center - half;
    const vec3 ab = half * 2.f;
    const f32 denom = ab.dot(ab);
    const f32 s = denom > 1e-12f ? std::clamp((p - a).dot(ab) / denom, 0.f, 1.f) : 0.f;
    return (p - (a + ab * s)).length() - params.x;
}

} // namespace

void PhysicsManager::init(const PhysicsManagerDesc& desc) {
    destroy();
    m_desc = desc;
    m_desc.solver.enableCcd = desc.enableCcd; // CCD runs inside the solver step (B4.6)
    m_soa_.reserve(std::min(desc.maxBodies, 65536u));
    m_solver_.init(desc.maxBodies, desc.maxContacts, desc.maxConstraints);
    m_initialized = true;
}

void PhysicsManager::destroy() {
    m_solver_.destroy();
    m_soa_.clear();
    m_shapes_.clear();
    m_bodyToEntity_.clear();
    m_entityToBodyIdx_.clear();
    m_writtenPositions_.clear();
    m_writtenVelocities_.clear();
    m_writtenOrientations_.clear();
    m_writtenAngularVelocities_.clear();
    m_kinematicTargets_.clear();
    m_activePairs_.clear();
    m_currentPairs_.clear();
    m_lastEvents_.clear();
    m_destructionEvents_.clear();
    m_destructibles_.clear();
    m_lastDebris_.clear();
    m_joints_.clear();
    m_freeJointSlots_.clear();
    m_solverJoints_.clear();
    m_solverJointSlots_.clear();
    m_pendingWakes_.clear();
    m_jointEvents_.clear();
    m_liveJointCount_ = 0;
    m_stepCount = 0;
    m_lastCcdHitCount_ = 0;
    m_initialized = false;
}

u32 PhysicsManager::bodyIndex(fuse::ecs::EntityID id) const {
    if (!id.valid()) {
        return kNoBody;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end() || m_bodyToEntity_[it->second] != id) {
        return kNoBody;
    }
    return it->second;
}

void PhysicsManager::step(fuse::ecs::Registry& registry, f32 dt, PhysicsStreamManager& streams) {
    (void)streams;
    if (!m_initialized || dt <= 0.f) {
        return;
    }
    syncEcsToSoa_(registry, dt);
    for (const fuse::ecs::EntityID id : m_pendingWakes_) {
        const u32 body = bodyIndex(id);
        if (body != kNoBody) {
            wake(m_soa_, body);
        }
    }
    m_pendingWakes_.clear();
    buildSolverJoints_();
    m_solver_.step(m_soa_, m_shapes_, m_desc.solver, dt);
    collectJointBreaks_();
    m_lastCcdHitCount_ = m_solver_.lastCcdHitCount();
    m_kinematicTargets_.clear();
    syncSoaToEcs_(registry);
    raiseEvents_();

    m_lastDebris_.clear();
    if (m_desc.enableDestruction && !m_destructionEvents_.empty()) {
        processDestructionEvents_(registry);
    }
    m_destructionEvents_.clear();
    ++m_stepCount;
}

void PhysicsManager::syncEcsToSoa_(fuse::ecs::Registry& registry, f32 dt) {
    using namespace fuse::ecs;
    m_seen_.assign(m_soa_.count(), 0u);
    registry.each<Transform, fuse::ecs::RigidBody, Collider>([&](EntityID id, Transform& transform,
                                                                 fuse::ecs::RigidBody& rb, Collider& collider) {
        const vec3 position = toPhysics(transform.position);
        const vec3 velocity = toPhysics(rb.velocity);
        const quat orientation = quatNormalize(toPhysics(transform.rotation));
        const vec3 angularVelocity = toPhysics(rb.angular_velocity);
        u32 body = bodyIndex(id);
        const bool created = body == kNoBody;
        if (created) {
            body = m_soa_.addBody(position, 0.f);
            m_shapes_.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
            m_soa_.linearVelocities[body] = velocity;
            // Keep a (near-)unit saved rotation bit-exact — the solver's own orientations are only
            // renormalised when they drift — so a reloaded scene resumes exactly where it was saved.
            const quat saved = toPhysics(transform.rotation);
            const f32 norm2 = saved.x * saved.x + saved.y * saved.y + saved.z * saved.z + saved.w * saved.w;
            const quat initial = std::fabs(norm2 - 1.f) < 1e-4f ? saved : orientation;
            m_soa_.orientations[body] = initial;
            m_soa_.predictedOrientations[body] = initial;
            m_soa_.angularVelocities[body] = angularVelocity;
            if (rb.is_sleeping) {
                m_soa_.flags[body] |= RB_SLEEPING;
            }
            // Resume the sleep countdown written back by syncSoaToEcs_ (saved scenes, re-added bodies).
            m_soa_.sleepTimers[body] = rb.sleep_timer;
            m_bodyToEntity_.push_back(id);
            m_entityToBodyIdx_[id.index] = body;
            m_writtenPositions_.push_back(position);
            m_writtenVelocities_.push_back(velocity);
            m_writtenOrientations_.push_back(toPhysics(transform.rotation));
            m_writtenAngularVelocities_.push_back(angularVelocity);
            m_seen_.push_back(0u);
        }
        m_seen_[body] = 1u;

        const auto target = m_kinematicTargets_.find(id.index);
        const bool kinematic = registry.has<TagKinematic>(id) || target != m_kinematicTargets_.end();
        u32 flags = m_soa_.flags[body] & RB_SLEEPING;
        flags |= rb.is_static ? RB_STATIC : 0u;
        flags |= kinematic ? RB_KINEMATIC : 0u;
        flags |= collider.is_trigger ? RB_TRIGGER : 0u;
        flags |= collider.ccd ? RB_CCD : 0u;
        if (rb.is_static || kinematic) {
            flags &= ~RB_SLEEPING;
        }
        m_soa_.flags[body] = flags;

        if (kinematic) {
            // Sweep from the current pose to the programmed one over this step.
            const vec3 goal = target != m_kinematicTargets_.end() ? target->second.position : position;
            const quat goalOrientation =
                target != m_kinematicTargets_.end() ? quatNormalize(target->second.orientation) : orientation;
            m_soa_.linearVelocities[body] = (goal - m_soa_.positions[body]) * (1.f / dt);
            m_soa_.angularVelocities[body] = angularVelocityBetween(m_soa_.orientations[body], goalOrientation, dt);
        } else {
            if (!sameVec(position, m_writtenPositions_[body])) { // teleported by game code
                m_soa_.positions[body] = position;
                m_soa_.predictedPositions[body] = position;
                wake(m_soa_, body);
            }
            if (!sameVec(velocity, m_writtenVelocities_[body])) { // velocity written by game code
                m_soa_.linearVelocities[body] = velocity;
                wake(m_soa_, body);
            }
            if (!sameQuat(toPhysics(transform.rotation), m_writtenOrientations_[body])) { // rotated by game code
                m_soa_.orientations[body] = orientation;
                m_soa_.predictedOrientations[body] = orientation;
                wake(m_soa_, body);
            }
            if (!sameVec(angularVelocity, m_writtenAngularVelocities_[body])) {
                m_soa_.angularVelocities[body] = angularVelocity;
                wake(m_soa_, body);
            }
        }

        const f32 invMass = (rb.is_static || kinematic || rb.mass <= 0.f) ? 0.f : 1.f / rb.mass;
        if (!created && invMass != m_soa_.invMasses[body] && invMass > 0.f) {
            wake(m_soa_, body); // mass edited by game code: joint and contact loads change
        }
        m_soa_.invMasses[body] = invMass;
        m_soa_.restitutions[body] = rb.restitution;
        m_soa_.frictionStatic[body] = collider.friction_static;
        m_soa_.frictionDynamic[body] = collider.friction_dynamic;
        m_soa_.collisionLayers[body] = collider.layer;
        m_soa_.collisionMasks[body] = collider.mask;
        const vec3 force = toPhysics(rb.force_accumulator);
        if (force.dot(force) > 0.f) {
            m_soa_.forces[body] += force;
            rb.force_accumulator = {};
        }
        const vec3 torque = toPhysics(rb.torque_accumulator);
        if (torque.dot(torque) > 0.f) {
            m_soa_.torques[body] += torque;
            rb.torque_accumulator = {};
        }
        m_shapes_.types[body] = collider.shape;
        m_shapes_.params[body] = toPhysics(collider.params);
        m_shapes_.scalars[body] = collider.scalar;
    });

    // Entities destroyed or stripped of a physics component leave the simulation.
    for (u32 body = m_soa_.count(); body-- > 0;) {
        if (m_seen_[body] == 0u) {
            removeBody_(body);
        }
    }
}

void PhysicsManager::removeBody_(u32 body) {
    const u32 last = m_soa_.count() - 1u;
    m_entityToBodyIdx_.erase(m_bodyToEntity_[body].index);
    if (body != last) {
        m_bodyToEntity_[body] = m_bodyToEntity_[last];
        m_entityToBodyIdx_[m_bodyToEntity_[body].index] = body;
        m_writtenPositions_[body] = m_writtenPositions_[last];
        m_writtenVelocities_[body] = m_writtenVelocities_[last];
        m_writtenOrientations_[body] = m_writtenOrientations_[last];
        m_writtenAngularVelocities_[body] = m_writtenAngularVelocities_[last];
        m_seen_[body] = m_seen_[last];
    }
    m_soa_.removeBodySwap(body);
    m_shapes_.removeShapeSwap(body);
    if (body < m_shapes_.count()) {
        m_shapes_.bodyIndices[body] = body;
    }
    m_bodyToEntity_.pop_back();
    m_writtenPositions_.pop_back();
    m_writtenVelocities_.pop_back();
    m_writtenOrientations_.pop_back();
    m_writtenAngularVelocities_.pop_back();
    m_seen_.pop_back();
}

void PhysicsManager::syncSoaToEcs_(fuse::ecs::Registry& registry) {
    using namespace fuse::ecs;
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        const EntityID id = m_bodyToEntity_[body];
        Transform* transform = registry.get<Transform>(id);
        fuse::ecs::RigidBody* rb = registry.get<fuse::ecs::RigidBody>(id);
        if (transform == nullptr || rb == nullptr) {
            continue;
        }
        const vec3 position = m_soa_.positions[body];
        const vec3 velocity = m_soa_.linearVelocities[body];
        const quat orientation = m_soa_.orientations[body];
        const vec3 angularVelocity = m_soa_.angularVelocities[body];
        if (!sameVec(position, toPhysics(transform->position))) {
            transform->position = toEcs(position, 1.f);
            transform->dirty = true;
        }
        if (!sameQuat(orientation, toPhysics(transform->rotation))) {
            transform->rotation = toEcs(orientation);
            transform->dirty = true;
        }
        rb->velocity = toEcs(velocity, 0.f);
        rb->angular_velocity = toEcs(angularVelocity, 0.f);
        rb->is_sleeping = (m_soa_.flags[body] & RB_SLEEPING) != 0u;
        rb->sleep_timer = m_soa_.sleepTimers[body];
        m_writtenPositions_[body] = position;
        m_writtenVelocities_[body] = velocity;
        m_writtenOrientations_[body] = orientation;
        m_writtenAngularVelocities_[body] = angularVelocity;
    }
}

void PhysicsManager::raiseEvents_() {
    m_lastEvents_.clear();
    m_currentPairs_.clear();
    for (const FrameContact& contact : m_solver_.frameContacts()) {
        const fuse::ecs::EntityID a = m_bodyToEntity_[contact.bodyA];
        const fuse::ecs::EntityID b = m_bodyToEntity_[contact.bodyB];
        const u64 key = pairKey(a, b);
        m_currentPairs_[key] = {a, b, contact.trigger};
        CollisionEvent event{};
        event.entityA = a;
        event.entityB = b;
        event.contactPoint = contact.point;
        event.contactNormal = contact.normal;
        event.impulse = contact.impulse;
        const PairKey* previous = m_activePairs_.find(key);
        if (previous == nullptr || (previous->a != a && previous->a != b)) {
            event.type = contact.trigger ? CollisionEventType::Trigger : CollisionEventType::Enter;
            m_lastEvents_.push_back(event);
        } else if (!contact.trigger) {
            event.type = CollisionEventType::Stay;
            m_lastEvents_.push_back(event);
        }
    }
    m_activePairs_.for_each([this](u64 key, const PairKey& pair) {
        const PairKey* now = m_currentPairs_.find(key);
        if (now == nullptr || (now->a != pair.a && now->a != pair.b)) {
            if (bodyIndex(pair.a) == kNoBody || bodyIndex(pair.b) == kNoBody) {
                return; // an entity was destroyed / left the simulation: never name it in an event
            }
            CollisionEvent event{};
            event.type = CollisionEventType::Exit;
            event.entityA = pair.a;
            event.entityB = pair.b;
            m_lastEvents_.push_back(event);
        }
    });
    m_activePairs_.swap(m_currentPairs_);
    m_lastEvents_.insert(m_lastEvents_.end(), m_jointEvents_.begin(), m_jointEvents_.end());
    m_jointEvents_.clear();
    m_collisionEvents_.dispatch(m_lastEvents_);
}

bool PhysicsManager::rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal,
                             f32& t, const fuse::ecs::Registry* aliveIn) const {
    if (direction.length() < 1e-6f || maxT <= 0.f) {
        return false;
    }
    const vec3 dir = direction.normalized();
    f32 best = maxT;
    bool found = false;
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if ((m_soa_.flags[body] & RB_TRIGGER) != 0u) {
            continue;
        }
        if (aliveIn != nullptr && !aliveIn->alive(m_bodyToEntity_[body])) {
            continue; // destroyed since the last step; its body leaves on the next one
        }
        const vec3 center = m_soa_.positions[body];
        const vec3 params = m_shapes_.params[body];
        const quat rotation = m_soa_.orientations[body];
        f32 tHit = 0.f;
        vec3 n{};
        bool ok = false;
        switch (static_cast<CollisionShapeType>(m_shapes_.types[body])) {
        case CollisionShapeType::Sphere:
            ok = raySphere(origin, dir, center, params.x, tHit);
            n = ok ? (origin + dir * tHit - center).normalized() : n;
            break;
        case CollisionShapeType::Box: {
            // Slab test in the box frame; the hit normal is rotated back to world space.
            const vec3 localOrigin = inverseRotate(rotation, origin - center);
            const vec3 localDir = inverseRotate(rotation, dir);
            ok = rayAabb(localOrigin, localDir, params * -1.f, params, tHit, n);
            n = rotate(rotation, n);
            break;
        }
        case CollisionShapeType::Capsule: {
            // Sphere tracing on the capsule distance (exact distance => never overshoots).
            f32 travelled = 0.f;
            for (int i = 0; i < 96 && travelled <= best; ++i) {
                const f32 d = capsuleDistance(origin + dir * travelled, center, params, rotation);
                if (d < 1e-4f) {
                    ok = true;
                    tHit = travelled;
                    const vec3 p = origin + dir * travelled;
                    const f32 h = 1e-3f;
                    n = vec3{capsuleDistance(p + vec3{h, 0.f, 0.f}, center, params, rotation) -
                                 capsuleDistance(p - vec3{h, 0.f, 0.f}, center, params, rotation),
                             capsuleDistance(p + vec3{0.f, h, 0.f}, center, params, rotation) -
                                 capsuleDistance(p - vec3{0.f, h, 0.f}, center, params, rotation),
                             capsuleDistance(p + vec3{0.f, 0.f, h}, center, params, rotation) -
                                 capsuleDistance(p - vec3{0.f, 0.f, h}, center, params, rotation)}
                            .normalized();
                    break;
                }
                travelled += d;
            }
            break;
        }
        case CollisionShapeType::Plane: {
            const f32 denom = params.dot(dir);
            if (std::fabs(denom) > 1e-9f) {
                tHit = (m_shapes_.scalars[body] - params.dot(origin)) / denom;
                ok = tHit >= 0.f;
                n = denom < 0.f ? params : params * -1.f;
            }
            break;
        }
        default:
            break;
        }
        if (ok && tHit <= best) {
            best = tHit;
            hit = m_bodyToEntity_[body];
            normal = n;
            found = true;
        }
    }
    if (found) {
        t = best;
    }
    return found;
}

void PhysicsManager::querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results,
                                 const fuse::ecs::Registry* aliveIn) const {
    if (radius <= 0.f) {
        return;
    }
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if (aliveIn != nullptr && !aliveIn->alive(m_bodyToEntity_[body])) {
            continue;
        }
        const vec3 p = m_soa_.positions[body];
        const vec3 params = m_shapes_.params[body];
        bool overlap = false;
        switch (static_cast<CollisionShapeType>(m_shapes_.types[body])) {
        case CollisionShapeType::Sphere:
            overlap = (center - p).length() <= radius + params.x;
            break;
        case CollisionShapeType::Box: {
            const vec3 local = inverseRotate(m_soa_.orientations[body], center - p);
            const vec3 closest{std::clamp(local.x, -params.x, params.x), std::clamp(local.y, -params.y, params.y),
                               std::clamp(local.z, -params.z, params.z)};
            overlap = (local - closest).length() <= radius;
            break;
        }
        case CollisionShapeType::Capsule:
            overlap = capsuleDistance(center, p, params, m_soa_.orientations[body]) <= radius;
            break;
        case CollisionShapeType::Plane:
            overlap = params.dot(center) - m_shapes_.scalars[body] <= radius;
            break;
        default:
            break;
        }
        if (overlap) {
            results.push_back(m_bodyToEntity_[body]);
        }
    }
}

bool PhysicsManager::isSleeping(fuse::ecs::EntityID id) const {
    const u32 body = bodyIndex(id);
    return body != kNoBody && (m_soa_.flags[body] & RB_SLEEPING) != 0u;
}

void PhysicsManager::applyImpulse(fuse::ecs::EntityID id, vec3 impulse) {
    const u32 body = bodyIndex(id);
    if (body == kNoBody || m_soa_.invMasses[body] <= 0.f) {
        return;
    }
    m_soa_.linearVelocities[body] += impulse * m_soa_.invMasses[body];
    wake(m_soa_, body);
}

void PhysicsManager::applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint) {
    const u32 body = bodyIndex(id);
    if (body == kNoBody || m_soa_.invMasses[body] <= 0.f) {
        return;
    }
    m_soa_.linearVelocities[body] += impulse * m_soa_.invMasses[body];
    if ((m_soa_.flags[body] & RB_FIXED_ROTATION) == 0u) {
        const vec3 invInertia = shapeInverseInertia(static_cast<CollisionShapeType>(m_shapes_.types[body]),
                                                    m_shapes_.params[body], m_soa_.invMasses[body]);
        const vec3 arm = worldPoint - m_soa_.positions[body];
        m_soa_.angularVelocities[body] += applyInverseInertia(m_soa_.orientations[body], invInertia, arm.cross(impulse));
    }
    wake(m_soa_, body);
}

void PhysicsManager::applyForce(fuse::ecs::EntityID id, vec3 force) {
    const u32 body = bodyIndex(id);
    if (body == kNoBody || m_soa_.invMasses[body] <= 0.f) {
        return;
    }
    m_soa_.forces[body] += force;
    wake(m_soa_, body);
}

void PhysicsManager::applyTorque(fuse::ecs::EntityID id, vec3 torque) {
    const u32 body = bodyIndex(id);
    if (body == kNoBody || m_soa_.invMasses[body] <= 0.f) {
        return;
    }
    m_soa_.torques[body] += torque;
    wake(m_soa_, body);
}

void PhysicsManager::setVelocity(fuse::ecs::EntityID id, vec3 linear, vec3 angular) {
    const u32 body = bodyIndex(id);
    if (body == kNoBody) {
        return;
    }
    m_soa_.linearVelocities[body] = linear;
    m_soa_.angularVelocities[body] = angular;
    wake(m_soa_, body);
}

void PhysicsManager::setKinematicTarget(fuse::ecs::EntityID id, vec3 position, quat orientation) {
    if (id.valid()) {
        m_kinematicTargets_[id.index] = {position, orientation};
    }
}

void PhysicsManager::pushDestructionEvent(const DestructionEvent& event) {
    m_destructionEvents_.push_back(event);
}

void PhysicsManager::addDestructible(fuse::ecs::EntityID entity, const VoxelVolume& volume,
                                     const VoxelMaterial& material) {
    if (entity.valid()) {
        m_destructibles_[entity.index] = {volume, material};
    }
}

DestructibleVolume* PhysicsManager::destructible(fuse::ecs::EntityID entity) {
    const auto it = m_destructibles_.find(entity.index);
    return it == m_destructibles_.end() ? nullptr : &it->second;
}

void PhysicsManager::processDestructionEvents_(fuse::ecs::Registry& registry) {
    // Debris entities join the simulation on the next step's sync.
    DestructionSystem::processEvents(m_destructionEvents_, m_destructibles_, registry, m_lastDebris_);
}

// --- Joints ---------------------------------------------------------------------------------------

JointDesc JointDesc::ballSocket(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot, vec3 twistAxis) {
    JointDesc desc;
    desc.type = JointType::BallSocket;
    desc.entityA = a;
    desc.entityB = b;
    desc.anchorA = pivot;
    desc.anchorB = pivot;
    desc.axis = twistAxis;
    return desc;
}

JointDesc JointDesc::hinge(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot, vec3 axis) {
    JointDesc desc = ballSocket(a, b, pivot, axis);
    desc.type = JointType::Hinge;
    return desc;
}

JointDesc JointDesc::fixed(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 pivot) {
    JointDesc desc = ballSocket(a, b, pivot);
    desc.type = JointType::Fixed;
    return desc;
}

JointDesc JointDesc::distance(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB,
                              f32 minLength, f32 maxLength) {
    JointDesc desc;
    desc.type = JointType::Distance;
    desc.entityA = a;
    desc.entityB = b;
    desc.anchorA = anchorA;
    desc.anchorB = anchorB;
    desc.minDistance = minLength;
    desc.maxDistance = maxLength;
    return desc;
}

JointDesc JointDesc::rope(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB, f32 maxLength) {
    return distance(a, b, anchorA, anchorB, 0.f, maxLength);
}

JointDesc JointDesc::spring(fuse::ecs::EntityID a, fuse::ecs::EntityID b, vec3 anchorA, vec3 anchorB, f32 stiffness,
                            f32 damping, f32 restLength) {
    JointDesc desc = distance(a, b, anchorA, anchorB);
    desc.type = JointType::Spring;
    desc.stiffness = stiffness;
    desc.damping = damping;
    desc.restLength = restLength;
    return desc;
}

namespace {

bool entityPose(const fuse::ecs::Registry& registry, fuse::ecs::EntityID id, vec3& position, quat& orientation) {
    if (!id.valid()) {
        position = {};
        orientation = {};
        return true; // the world frame
    }
    if (!registry.alive(id)) {
        return false;
    }
    const fuse::ecs::Transform* transform = registry.get<fuse::ecs::Transform>(id);
    if (transform == nullptr) {
        return false;
    }
    position = toPhysics(transform->position);
    orientation = quatNormalize(toPhysics(transform->rotation));
    return true;
}

vec3 perpendicularTo(vec3 axis, vec3 hint) {
    vec3 normal = hint - axis * axis.dot(hint);
    if (normal.length() < 1e-4f) {
        const vec3 fallback = std::fabs(axis.x) < 0.9f ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f};
        normal = fallback - axis * axis.dot(fallback);
    }
    return normal.normalized();
}

} // namespace

JointHandle PhysicsManager::createJoint(const fuse::ecs::Registry& registry, const JointDesc& desc) {
    vec3 positionA{};
    vec3 positionB{};
    quat orientationA{};
    quat orientationB{};
    if (!desc.entityA.valid() || desc.entityA == desc.entityB ||
        !entityPose(registry, desc.entityA, positionA, orientationA) ||
        !entityPose(registry, desc.entityB, positionB, orientationB)) {
        return {};
    }
    JointConstraint joint{};
    joint.type = desc.type;
    joint.bodyA = kNoBody; // mapped every step
    joint.bodyB = kJointWorldBody;
    const bool pointJoint = desc.type == JointType::BallSocket || desc.type == JointType::Hinge ||
                            desc.type == JointType::Fixed;
    const vec3 anchorB = pointJoint ? desc.anchorA : desc.anchorB;
    joint.localAnchorA = inverseRotate(orientationA, desc.anchorA - positionA);
    joint.localAnchorB = inverseRotate(orientationB, anchorB - positionB);
    const vec3 axis = desc.axis.normalized();
    const vec3 normal = perpendicularTo(axis, desc.normal);
    joint.localAxisA = inverseRotate(orientationA, axis);
    joint.localAxisB = inverseRotate(orientationB, axis);
    joint.localNormalA = inverseRotate(orientationA, normal);
    joint.localNormalB = inverseRotate(orientationB, normal);
    joint.restRelative = quatNormalize(quatMul(quatConjugate(orientationA), orientationB));
    joint.hingeLimit = desc.hingeLimit;
    joint.minAngle = desc.minAngle;
    joint.maxAngle = desc.maxAngle;
    joint.swingLimit = desc.swingLimit;
    joint.twistLimit = desc.twistLimit;
    joint.minTwist = desc.minTwist;
    joint.maxTwist = desc.maxTwist;
    const f32 current = (desc.anchorA - anchorB).length();
    joint.minDistance = desc.minDistance < 0.f ? current : desc.minDistance;
    joint.maxDistance = desc.maxDistance < 0.f ? current : std::max(desc.maxDistance, joint.minDistance);
    joint.restLength = desc.restLength < 0.f ? current : desc.restLength;
    joint.damping = std::max(desc.damping, 0.f);
    joint.compliance = desc.type == JointType::Spring ? (desc.stiffness > 0.f ? 1.f / desc.stiffness : 0.f)
                                                      : std::max(desc.compliance, 0.f);
    joint.angularCompliance = std::max(desc.angularCompliance, 0.f);
    joint.breakForce = desc.breakForce;
    joint.breakTorque = desc.breakTorque;
    joint.collideConnected = desc.collideConnected;

    u32 slot = 0;
    if (!m_freeJointSlots_.empty()) {
        slot = m_freeJointSlots_.back();
        m_freeJointSlots_.pop_back();
    } else {
        slot = static_cast<u32>(m_joints_.size());
        m_joints_.push_back({});
    }
    JointRecord& record = m_joints_[slot];
    record.constraint = joint;
    record.entityA = desc.entityA;
    record.entityB = desc.entityB;
    record.alive = true;
    record.broken = false;
    record.lastForce = 0.f;
    record.lastTorque = 0.f;
    ++m_liveJointCount_;
    wakeEntity_(desc.entityA);
    wakeEntity_(desc.entityB);
    return {slot, record.generation};
}

PhysicsManager::JointRecord* PhysicsManager::jointRecord_(JointHandle handle) {
    if (!handle.valid() || handle.index >= m_joints_.size()) {
        return nullptr;
    }
    JointRecord& record = m_joints_[handle.index];
    return record.alive && record.generation == handle.generation ? &record : nullptr;
}

const PhysicsManager::JointRecord* PhysicsManager::jointRecord_(JointHandle handle) const {
    return const_cast<PhysicsManager*>(this)->jointRecord_(handle);
}

void PhysicsManager::wakeEntity_(fuse::ecs::EntityID id) {
    if (!id.valid()) {
        return;
    }
    const u32 body = bodyIndex(id);
    if (body != kNoBody) {
        wake(m_soa_, body);
    }
    m_pendingWakes_.push_back(id); // also once the body is in the simulation
}

void PhysicsManager::releaseJoint_(u32 slot) {
    JointRecord& record = m_joints_[slot];
    wakeEntity_(record.entityA);
    wakeEntity_(record.entityB);
    record.alive = false;
    record.broken = false;
    ++record.generation;
    if (record.generation == 0u) {
        record.generation = 1u;
    }
    m_freeJointSlots_.push_back(slot);
    --m_liveJointCount_;
}

bool PhysicsManager::destroyJoint(JointHandle handle) {
    if (jointRecord_(handle) == nullptr) {
        return false;
    }
    releaseJoint_(handle.index);
    return true;
}

bool PhysicsManager::isJointValid(JointHandle handle) const {
    return jointRecord_(handle) != nullptr;
}

bool PhysicsManager::isJointBroken(JointHandle handle) const {
    const JointRecord* record = jointRecord_(handle);
    return record != nullptr && record->broken;
}

bool PhysicsManager::setHingeLimits(JointHandle handle, bool enabled, f32 minAngle, f32 maxAngle) {
    JointRecord* record = jointRecord_(handle);
    if (record == nullptr) {
        return false;
    }
    record->constraint.hingeLimit = enabled;
    record->constraint.minAngle = std::min(minAngle, maxAngle);
    record->constraint.maxAngle = std::max(minAngle, maxAngle);
    wakeEntity_(record->entityA);
    wakeEntity_(record->entityB);
    return true;
}

bool PhysicsManager::setSwingTwistLimits(JointHandle handle, f32 swingLimit, bool twistEnabled, f32 minTwist,
                                         f32 maxTwist) {
    JointRecord* record = jointRecord_(handle);
    if (record == nullptr) {
        return false;
    }
    record->constraint.swingLimit = swingLimit;
    record->constraint.twistLimit = twistEnabled;
    record->constraint.minTwist = std::min(minTwist, maxTwist);
    record->constraint.maxTwist = std::max(minTwist, maxTwist);
    wakeEntity_(record->entityA);
    wakeEntity_(record->entityB);
    return true;
}

bool PhysicsManager::setBreakThresholds(JointHandle handle, f32 breakForce, f32 breakTorque) {
    JointRecord* record = jointRecord_(handle);
    if (record == nullptr) {
        return false;
    }
    record->constraint.breakForce = breakForce;
    record->constraint.breakTorque = breakTorque;
    wakeEntity_(record->entityA);
    wakeEntity_(record->entityB);
    return true;
}

const JointConstraint* PhysicsManager::joint(JointHandle handle) const {
    const JointRecord* record = jointRecord_(handle);
    return record != nullptr ? &record->constraint : nullptr;
}

f32 PhysicsManager::jointForce(JointHandle handle) const {
    const JointRecord* record = jointRecord_(handle);
    return record != nullptr ? record->lastForce : 0.f;
}

f32 PhysicsManager::jointTorque(JointHandle handle) const {
    const JointRecord* record = jointRecord_(handle);
    return record != nullptr ? record->lastTorque : 0.f;
}

bool PhysicsManager::jointAngles(JointHandle handle, f32& hingeAngle, f32& swing, f32& twist) const {
    const JointRecord* record = jointRecord_(handle);
    if (record == nullptr) {
        return false;
    }
    JointConstraint joint = record->constraint;
    joint.bodyA = bodyIndex(record->entityA);
    joint.bodyB = record->entityB.valid() ? bodyIndex(record->entityB) : kJointWorldBody;
    if (joint.bodyA == kNoBody || (record->entityB.valid() && joint.bodyB == kNoBody)) {
        return false;
    }
    hingeAngle = jointHingeAngle(m_soa_, joint);
    jointSwingTwist(m_soa_, joint, swing, twist);
    return true;
}

void PhysicsManager::buildSolverJoints_() {
    m_solverJoints_.clear();
    m_solverJointSlots_.clear();
    for (u32 slot = 0; slot < m_joints_.size(); ++slot) {
        JointRecord& record = m_joints_[slot];
        if (!record.alive) {
            continue;
        }
        const u32 bodyA = bodyIndex(record.entityA);
        const u32 bodyB = record.entityB.valid() ? bodyIndex(record.entityB) : kJointWorldBody;
        if (bodyA == kNoBody || (record.entityB.valid() && bodyB == kNoBody)) {
            releaseJoint_(slot); // an entity was destroyed or left the simulation
            continue;
        }
        record.constraint.bodyA = bodyA;
        record.constraint.bodyB = bodyB;
        if (record.broken) {
            record.lastForce = 0.f;
            record.lastTorque = 0.f;
            continue;
        }
        m_solverJoints_.push_back(record.constraint);
        m_solverJointSlots_.push_back(slot);
    }
    // Wakes raised by releases above apply before the solve.
    for (const fuse::ecs::EntityID id : m_pendingWakes_) {
        const u32 body = bodyIndex(id);
        if (body != kNoBody) {
            wake(m_soa_, body);
        }
    }
    m_pendingWakes_.clear();
    m_solver_.setJoints(m_solverJoints_);
}

void PhysicsManager::collectJointBreaks_() {
    const std::vector<JointSolveResult>& results = m_solver_.jointResults();
    for (u32 i = 0; i < m_solverJointSlots_.size() && i < results.size(); ++i) {
        const u32 slot = m_solverJointSlots_[i];
        JointRecord& record = m_joints_[slot];
        record.lastForce = results[i].force;
        record.lastTorque = results[i].torque;
        if (!results[i].broken || record.broken) {
            continue;
        }
        record.broken = true;
        CollisionEvent event{};
        event.type = CollisionEventType::JointBreak;
        event.entityA = record.entityA;
        event.entityB = record.entityB;
        const u32 body = record.constraint.bodyA;
        event.contactPoint = m_soa_.positions[body] + rotate(m_soa_.orientations[body], record.constraint.localAnchorA);
        event.impulse = results[i].force;
        event.jointIndex = slot;
        event.jointGeneration = record.generation;
        m_jointEvents_.push_back(event);
        wakeEntity_(record.entityA);
        wakeEntity_(record.entityB);
    }
}

} // namespace fuse::physics
