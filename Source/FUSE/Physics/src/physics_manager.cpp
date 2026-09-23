#include <fuse/physics/physics_manager.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>

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

bool sameVec(vec3 a, vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
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

f32 capsuleDistance(vec3 p, vec3 center, vec3 params) {
    const vec3 a = center - vec3{0.f, params.y, 0.f};
    const vec3 ab{0.f, 2.f * params.y, 0.f};
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
    m_kinematicTargets_.clear();
    m_activePairs_.clear();
    m_currentPairs_.clear();
    m_lastEvents_.clear();
    m_destructionEvents_.clear();
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
    m_solver_.step(m_soa_, m_shapes_, m_desc.solver, dt);
    m_lastCcdHitCount_ = m_solver_.lastCcdHitCount();
    m_kinematicTargets_.clear();
    syncSoaToEcs_(registry);
    raiseEvents_();

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
        u32 body = bodyIndex(id);
        if (body == kNoBody) {
            body = m_soa_.addBody(position, 0.f);
            m_shapes_.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
            m_soa_.linearVelocities[body] = velocity;
            if (rb.is_sleeping) {
                m_soa_.flags[body] |= RB_SLEEPING;
            }
            m_bodyToEntity_.push_back(id);
            m_entityToBodyIdx_[id.index] = body;
            m_writtenPositions_.push_back(position);
            m_writtenVelocities_.push_back(velocity);
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
            const vec3 goal = target != m_kinematicTargets_.end() ? target->second : position;
            m_soa_.linearVelocities[body] = (goal - m_soa_.positions[body]) * (1.f / dt);
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
        }

        m_soa_.invMasses[body] = (rb.is_static || kinematic || rb.mass <= 0.f) ? 0.f : 1.f / rb.mass;
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
        if (!sameVec(position, toPhysics(transform->position))) {
            transform->position = toEcs(position, 1.f);
            transform->dirty = true;
        }
        rb->velocity = toEcs(velocity, 0.f);
        rb->is_sleeping = (m_soa_.flags[body] & RB_SLEEPING) != 0u;
        rb->sleep_timer = m_soa_.sleepTimers[body];
        m_writtenPositions_[body] = position;
        m_writtenVelocities_[body] = velocity;
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
        const auto previous = m_activePairs_.find(key);
        if (previous == m_activePairs_.end() || previous->second.a != a && previous->second.a != b) {
            event.type = contact.trigger ? CollisionEventType::Trigger : CollisionEventType::Enter;
            m_lastEvents_.push_back(event);
        } else if (!contact.trigger) {
            event.type = CollisionEventType::Stay;
            m_lastEvents_.push_back(event);
        }
    }
    for (const auto& [key, pair] : m_activePairs_) {
        const auto now = m_currentPairs_.find(key);
        if (now == m_currentPairs_.end() || (now->second.a != pair.a && now->second.a != pair.b)) {
            CollisionEvent event{};
            event.type = CollisionEventType::Exit;
            event.entityA = pair.a;
            event.entityB = pair.b;
            m_lastEvents_.push_back(event);
        }
    }
    std::swap(m_activePairs_, m_currentPairs_);
    m_collisionEvents_.dispatch(m_lastEvents_);
}

bool PhysicsManager::rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal,
                             f32& t) const {
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
        const vec3 center = m_soa_.positions[body];
        const vec3 params = m_shapes_.params[body];
        f32 tHit = 0.f;
        vec3 n{};
        bool ok = false;
        switch (static_cast<CollisionShapeType>(m_shapes_.types[body])) {
        case CollisionShapeType::Sphere:
            ok = raySphere(origin, dir, center, params.x, tHit);
            n = ok ? (origin + dir * tHit - center).normalized() : n;
            break;
        case CollisionShapeType::Box:
            ok = rayAabb(origin, dir, center - params, center + params, tHit, n);
            break;
        case CollisionShapeType::Capsule: {
            // Sphere tracing on the capsule distance (exact distance => never overshoots).
            f32 travelled = 0.f;
            for (int i = 0; i < 96 && travelled <= best; ++i) {
                const f32 d = capsuleDistance(origin + dir * travelled, center, params);
                if (d < 1e-4f) {
                    ok = true;
                    tHit = travelled;
                    const vec3 p = origin + dir * travelled;
                    const f32 h = 1e-3f;
                    n = vec3{capsuleDistance(p + vec3{h, 0.f, 0.f}, center, params) -
                                 capsuleDistance(p - vec3{h, 0.f, 0.f}, center, params),
                             capsuleDistance(p + vec3{0.f, h, 0.f}, center, params) -
                                 capsuleDistance(p - vec3{0.f, h, 0.f}, center, params),
                             capsuleDistance(p + vec3{0.f, 0.f, h}, center, params) -
                                 capsuleDistance(p - vec3{0.f, 0.f, h}, center, params)}
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

void PhysicsManager::querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const {
    if (radius <= 0.f) {
        return;
    }
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        const vec3 p = m_soa_.positions[body];
        const vec3 params = m_shapes_.params[body];
        bool overlap = false;
        switch (static_cast<CollisionShapeType>(m_shapes_.types[body])) {
        case CollisionShapeType::Sphere:
            overlap = (center - p).length() <= radius + params.x;
            break;
        case CollisionShapeType::Box: {
            const vec3 closest{std::clamp(center.x, p.x - params.x, p.x + params.x),
                               std::clamp(center.y, p.y - params.y, p.y + params.y),
                               std::clamp(center.z, p.z - params.z, p.z + params.z)};
            overlap = (center - closest).length() <= radius;
            break;
        }
        case CollisionShapeType::Capsule:
            overlap = capsuleDistance(center, p, params) <= radius;
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

void PhysicsManager::applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint) {
    (void)worldPoint; // bodies carry no rotational state yet: the impulse acts on the centre of mass
    const u32 body = bodyIndex(id);
    if (body == kNoBody || m_soa_.invMasses[body] <= 0.f) {
        return;
    }
    m_soa_.linearVelocities[body] += impulse * m_soa_.invMasses[body];
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
    (void)orientation; // orientation is not simulated yet
    if (id.valid()) {
        m_kinematicTargets_[id.index] = position;
    }
}

void PhysicsManager::pushDestructionEvent(const DestructionEvent& event) {
    m_destructionEvents_.push_back(event);
}

void PhysicsManager::processDestructionEvents_(fuse::ecs::Registry& registry) {
    (void)registry;
    PhysicsRegistry legacy{};
    PhysicsResourceManager resources{};
    DestructionSystem::processEvents(m_destructionEvents_, legacy, resources);
}

} // namespace fuse::physics
