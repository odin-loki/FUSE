#include <fuse/physics/physics_manager.hpp>

#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/character_controller.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_cluster.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

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

f32 capsuleDistance(vec3 p, vec3 center, vec3 params, const quat& orientation) {
    const vec3 half = capsuleHalfAxis(orientation, params.y);
    const vec3 a = center - half;
    const vec3 ab = half * 2.f;
    const f32 denom = ab.dot(ab);
    const f32 s = denom > 1e-12f ? std::clamp((p - a).dot(ab) / denom, 0.f, 1.f) : 0.f;
    return (p - (a + ab * s)).length() - params.x;
}

} // namespace

PhysicsManager::~PhysicsManager() {
    releaseDestructibleRefs_();
}

void PhysicsManager::init(const PhysicsManagerDesc& desc) {
    destroy();
    fuse::ecs::ComponentTypes::register_type<fuse::ecs::CharacterController>();
    m_desc = desc;
    m_desc.solver.enableCcd = desc.enableCcd; // CCD runs inside the solver step (B4.6)
    m_soa_.reserve(std::min(desc.maxBodies, 65536u));
    m_solver_.init(desc.maxBodies, desc.maxContacts, desc.maxConstraints);
    m_initialized = true;
}

void PhysicsManager::releaseDestructibleRefs_() {
    // Only views of this manager's own volumes (a copied manager re-registers its own on its next step).
    for (const auto& [key, info] : m_destructibleBodies_) {
        const auto it = m_destructibles_.find(key);
        if (info.shapeRef != kNoShapeRef && it != m_destructibles_.end() &&
            ShapePool::global().voxel(info.shapeRef) == &it->second.volume) {
            ShapePool::global().release(info.shapeRef);
        }
    }
    m_destructibleBodies_.clear();
}

void PhysicsManager::destroy() {
    releaseDestructibleRefs_();
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
    m_pairReserve_ = 0;
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
    // GAP-PHYS-CHARACTER: kinematic characters move first (against last step's world); a character that
    // is also a kinematic body then pushes rigid bodies from its new pose in this step's solve.
    if (m_desc.enableCharacterControllers) {
        m_characters_.update(registry, *this, dt);
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

        // Concave pooled shapes (triangle meshes, voxel volumes, SDFs) are static level geometry unless
        // driven kinematically.
        const CollisionShapeType shapeType = static_cast<CollisionShapeType>(collider.shape);
        const bool concave = shapeType == CollisionShapeType::TriMesh || shapeType == CollisionShapeType::Voxel ||
                             shapeType == CollisionShapeType::SdfMesh;
        if (concave && !kinematic) {
            m_soa_.flags[body] = (m_soa_.flags[body] | RB_STATIC) & ~RB_SLEEPING;
        }
        const bool immovable = rb.is_static || kinematic || rb.mass <= 0.f || (concave && !kinematic);
        const f32 invMass = immovable ? 0.f : 1.f / rb.mass;
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
        m_shapes_.scalars[body] = collider.scalar;
        if (isPooledShape(shapeType)) {
            // Hull / mesh / SDF / voxel geometry lives in the shape pool; params = its bounds.
            const u32 ref = resolveShapeRef_(id, collider.shape, collider.shape_asset, collider.shape_piece,
                                             collider.shape_ref);
            m_shapes_.shapeRefs[body] = ref;
            m_shapes_.params[body] = ShapePool::global().halfExtents(ref);
        } else {
            m_shapes_.shapeRefs[body] = kNoShapeRef;
            m_shapes_.params[body] = toPhysics(collider.params);
        }
    });
    syncDestructibleBodies_(registry);

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
    // Pre-size the pair maps and the event list for up to kPairsPerBody contact pairs per body (a
    // dense sphere/box pile stays below it), capped by maxContacts. Only a body-count increase
    // re-reserves, so contacts forming while a pile settles never rehash mid-simulation.
    constexpr usize kPairsPerBody = 8u;
    const usize pairTarget = std::min<usize>(m_desc.maxContacts, static_cast<usize>(m_soa_.count()) * kPairsPerBody);
    if (pairTarget > m_pairReserve_) {
        m_activePairs_.reserve(pairTarget);
        m_currentPairs_.reserve(pairTarget);
        m_lastEvents_.reserve(pairTarget * 2u);
        m_pairReserve_ = pairTarget;
    }
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
        RayHit rayHit{};
        if (rayCastShape(bodyShape(body), origin, dir, best, rayHit) && rayHit.t <= best) {
            best = rayHit.t;
            hit = m_bodyToEntity_[body];
            normal = rayHit.normal;
            found = true;
        }
    }
    if (found) {
        t = best;
    }
    return found;
}

narrowphase::ShapeInstance PhysicsManager::bodyShape(u32 body) const {
    narrowphase::ShapeInstance shape{};
    if (body >= m_soa_.count() || body >= m_shapes_.count()) {
        return shape;
    }
    shape.type = static_cast<CollisionShapeType>(m_shapes_.types[body]);
    shape.params = m_shapes_.params[body];
    shape.scalar = m_shapes_.scalars[body];
    shape.position = m_soa_.positions[body];
    shape.orientation = m_soa_.orientations[body];
    shape.shapeRef = m_shapes_.shapeRef(body);
    return shape;
}

vec3 PhysicsManager::pointVelocity(u32 body, vec3 point) const {
    if (body >= m_soa_.count() || (m_soa_.flags[body] & RB_STATIC) != 0u) {
        return {};
    }
    return m_soa_.linearVelocities[body] + m_soa_.angularVelocities[body].cross(point - m_soa_.positions[body]);
}

bool PhysicsManager::bodyPassesFilter_(u32 body, const QueryFilter& filter) const {
    const fuse::ecs::EntityID entity = m_bodyToEntity_[body];
    if (filter.ignore.valid() && entity == filter.ignore) {
        return false;
    }
    const u32 flags = m_soa_.flags[body];
    if ((flags & RB_TRIGGER) != 0u && !filter.includeTriggers) {
        return false;
    }
    if ((m_soa_.collisionLayers[body] & filter.layerMask) == 0u) {
        return false;
    }
    const bool dynamic = (flags & (RB_STATIC | RB_KINEMATIC)) == 0u && m_soa_.invMasses[body] > 0.f;
    if (dynamic && !filter.includeDynamic) {
        return false;
    }
    return filter.aliveIn == nullptr || filter.aliveIn->alive(entity);
}

bool PhysicsManager::rayCastFiltered(vec3 origin, vec3 direction, f32 maxT, ShapeCastResult& result,
                                     const QueryFilter& filter) const {
    if (direction.length() < 1e-6f || maxT <= 0.f) {
        return false;
    }
    const vec3 dir = direction.normalized();
    f32 best = maxT;
    bool found = false;
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if (!bodyPassesFilter_(body, filter)) {
            continue;
        }
        RayHit rayHit{};
        if (rayCastShape(bodyShape(body), origin, dir, best, rayHit) && rayHit.t <= best) {
            best = rayHit.t;
            result.entity = m_bodyToEntity_[body];
            result.body = body;
            result.hit = ShapeCastHit{};
            result.hit.distance = rayHit.t;
            result.hit.normal = rayHit.normal;
            result.hit.point = origin + dir * rayHit.t;
            found = true;
        }
    }
    return found;
}

bool PhysicsManager::shapeCast(const narrowphase::ShapeInstance& shape, vec3 direction, f32 maxDistance,
                               ShapeCastResult& result, const QueryFilter& filter) const {
    if (direction.dot(direction) < 1e-12f || maxDistance < 0.f) {
        return false;
    }
    const vec3 dir = direction.normalized();
    const aabb start = shapeInstanceBounds(shape);
    f32 best = maxDistance;
    bool found = false;
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if (!bodyPassesFilter_(body, filter)) {
            continue;
        }
        const narrowphase::ShapeInstance target = bodyShape(body);
        if (target.type != CollisionShapeType::Plane) {
            // Swept bounds of the caster against the body's bounds.
            const vec3 end = dir * best;
            const aabb other = shapeInstanceBounds(target);
            const f32 pad = 1e-3f;
            if (start.min.x + std::min(end.x, 0.f) > other.max.x + pad || start.max.x + std::max(end.x, 0.f) < other.min.x - pad ||
                start.min.y + std::min(end.y, 0.f) > other.max.y + pad || start.max.y + std::max(end.y, 0.f) < other.min.y - pad ||
                start.min.z + std::min(end.z, 0.f) > other.max.z + pad || start.max.z + std::max(end.z, 0.f) < other.min.z - pad) {
                continue;
            }
        }
        ShapeCastHit hit{};
        if (!physics::shapeCast(shape, dir, best, target, hit)) {
            continue;
        }
        const bool better = !found || hit.distance < result.hit.distance ||
                            (hit.startPenetrating && result.hit.startPenetrating && hit.penetration > result.hit.penetration);
        if (better) {
            result.entity = m_bodyToEntity_[body];
            result.body = body;
            result.hit = hit;
            best = hit.distance;
            found = true;
        }
    }
    return found;
}

u32 PhysicsManager::overlapShape(const narrowphase::ShapeInstance& shape, std::vector<OverlapContact>& out,
                                 const QueryFilter& filter) const {
    const u32 before = static_cast<u32>(out.size());
    const aabb bounds = shapeInstanceBounds(shape);
    narrowphase::ContactManifold manifolds[narrowphase::kMaxManifoldsPerPair];
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if (!bodyPassesFilter_(body, filter)) {
            continue;
        }
        const narrowphase::ShapeInstance target = bodyShape(body);
        if (target.type != CollisionShapeType::Plane) {
            const aabb other = shapeInstanceBounds(target);
            if (bounds.min.x > other.max.x || bounds.max.x < other.min.x || bounds.min.y > other.max.y ||
                bounds.max.y < other.min.y || bounds.min.z > other.max.z || bounds.max.z < other.min.z) {
                continue;
            }
        }
        const u32 count =
            narrowphase::collideShapesMulti(shape, target, 0u, 1u, 0.f, manifolds, narrowphase::kMaxManifoldsPerPair);
        for (u32 m = 0; m < count; ++m) {
            const f32 depth = manifolds[m].maxPenetration();
            if (!manifolds[m].valid || depth <= 0.f) {
                continue;
            }
            OverlapContact contact{};
            contact.entity = m_bodyToEntity_[body];
            contact.body = body;
            contact.normal = manifolds[m].contactNormal;
            contact.point = manifolds[m].contactPoint;
            contact.depth = depth;
            out.push_back(contact);
        }
    }
    return static_cast<u32>(out.size()) - before;
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
        default: {
            // Hulls, meshes, voxels, SDFs: the narrowphase (touching counts).
            narrowphase::ShapeInstance sphere{};
            sphere.type = CollisionShapeType::Sphere;
            sphere.params = {radius, 0.f, 0.f};
            sphere.position = center;
            narrowphase::ContactManifold manifolds[narrowphase::kMaxManifoldsPerPair];
            const u32 count = narrowphase::collideShapesMulti(sphere, bodyShape(body), 0u, 1u, 1e-5f, manifolds,
                                                              narrowphase::kMaxManifoldsPerPair);
            overlap = count > 0u;
            break;
        }
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
    if (!entity.valid()) {
        return;
    }
    DestructibleVolume& target = m_destructibles_[entity.index];
    target.volume = volume;
    target.material = material;
    DestructibleBody& info = m_destructibleBodies_[entity.index];
    info.entity = entity;
    destructibleRef_(entity.index);
    wakeAround_(volume.origin(), 1e30f); // the world changed: anything asleep may now be unsupported
}

u32 PhysicsManager::destructibleRef_(u32 key) {
    const auto it = m_destructibles_.find(key);
    if (it == m_destructibles_.end()) {
        return kNoShapeRef;
    }
    DestructibleBody& info = m_destructibleBodies_[key];
    ShapePool& pool = ShapePool::global();
    if (info.shapeRef == kNoShapeRef || pool.voxel(info.shapeRef) != &it->second.volume) {
        info.shapeRef = pool.addVoxelView(&it->second.volume);
    }
    return info.shapeRef;
}

u32 PhysicsManager::resolveShapeRef_(fuse::ecs::EntityID id, u32 shape, u64 asset, u32 piece, u32 ref) {
    const CollisionShapeType type = static_cast<CollisionShapeType>(shape);
    ShapePool& pool = ShapePool::global();
    if (ref != kNoShapeRef && pool.valid(ref) && pool.type(ref) == type) {
        return ref;
    }
    if (type == CollisionShapeType::Voxel) {
        const auto it = m_destructibleBodies_.find(id.index);
        if (it != m_destructibleBodies_.end() && it->second.entity == id) {
            return destructibleRef_(id.index);
        }
    }
    if (asset != 0u) {
        return pool.findAsset(asset, piece, type);
    }
    return kNoShapeRef;
}

void PhysicsManager::syncDestructibleBodies_(fuse::ecs::Registry& registry) {
    if (m_destructibleBodies_.empty()) {
        return;
    }
    // Entity order (the map's iteration order is unspecified): deterministic body indices.
    m_destructibleKeys_.clear();
    for (const auto& entry : m_destructibleBodies_) {
        m_destructibleKeys_.push_back(entry.first);
    }
    std::sort(m_destructibleKeys_.begin(), m_destructibleKeys_.end());
    for (const u32 key : m_destructibleKeys_) {
        const fuse::ecs::EntityID entity = m_destructibleBodies_[key].entity;
        if (!registry.alive(entity)) {
            continue; // leaves the simulation with its entity
        }
        const u32 ref = destructibleRef_(key);
        u32 body = bodyIndex(entity);
        if (body == kNoBody) {
            // The volume is in world space: a static body at the origin.
            body = m_soa_.addBody({}, 0.f, RB_STATIC);
            m_shapes_.addPooledShape(CollisionShapeType::Voxel, body, ref);
            m_bodyToEntity_.push_back(entity);
            m_entityToBodyIdx_[entity.index] = body;
            m_writtenPositions_.push_back({});
            m_writtenVelocities_.push_back({});
            m_writtenOrientations_.push_back({});
            m_writtenAngularVelocities_.push_back({});
            m_seen_.push_back(1u);
            continue;
        }
        if (m_seen_[body] == 0u) {
            // Our own body (the entity has no physics components of its own).
            m_seen_[body] = 1u;
            m_shapes_.types[body] = static_cast<u32>(CollisionShapeType::Voxel);
            m_shapes_.shapeRefs[body] = ref;
            m_shapes_.params[body] = ShapePool::global().halfExtents(ref);
        }
    }
}

void PhysicsManager::wakeAround_(vec3 center, f32 radius) {
    for (u32 body = 0; body < m_soa_.count(); ++body) {
        if ((m_soa_.flags[body] & RB_SLEEPING) == 0u) {
            continue;
        }
        const vec3 p = m_shapes_.params[body];
        const f32 reach = p.length() + std::max(p.x, 0.f);
        if ((m_soa_.positions[body] - center).length() <= radius + reach) {
            wake(m_soa_, body);
        }
    }
}

DestructibleVolume* PhysicsManager::destructible(fuse::ecs::EntityID entity) {
    const auto it = m_destructibles_.find(entity.index);
    return it == m_destructibles_.end() ? nullptr : &it->second;
}

void PhysicsManager::processDestructionEvents_(fuse::ecs::Registry& registry) {
    // Debris entities join the simulation on the next step's sync. The Voxel collision shapes view the
    // carved volumes directly (refreshed in place); bodies resting near a crater are woken so they fall.
    DestructionSystem::processEvents(m_destructionEvents_, m_destructibles_, registry, m_lastDebris_);
    for (const DestructionEvent& event : m_destructionEvents_) {
        const auto it = m_destructibles_.find(event.target.index);
        if (it == m_destructibles_.end()) {
            continue;
        }
        const f32 radius = event.carveRadius > 0.f
                               ? event.carveRadius
                               : DestructionSystem::deriveCarveRadius(event.impulse, it->second.material);
        wakeAround_(event.impactPoint, radius + 2.f * it->second.volume.voxelSize());
    }
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
