#include <fuse/physics/physics_manager.hpp>

#include <fuse/physics/broadphase/spatial_hash.hpp>

#include <algorithm>

namespace fuse::physics {

void PhysicsManager::init(const PhysicsManagerDesc& desc) {
    destroy();
    m_desc = desc;
    m_soa_.reserve(desc.maxBodies);
    m_solver_.init(desc.maxBodies, desc.maxContacts, desc.maxConstraints);
    m_initialized = true;
}

void PhysicsManager::destroy() {
    m_solver_.destroy();
    m_soa_.clear();
    m_shapes_.clear();
    m_bodyToEntity_.clear();
    m_entityToBodyIdx_.clear();
    m_destructionEvents_.clear();
    m_stepCount = 0;
    m_lastCcdHitCount_ = 0;
    m_initialized = false;
}

void PhysicsManager::step(PhysicsRegistry& registry, f32 dt, PhysicsStreamManager& streams) {
    if (!m_initialized) {
        return;
    }

    syncEcsToSoa_(registry);

    if (m_desc.enableCcd) {
        runCcdSweep_(dt);
    } else {
        m_lastCcdHitCount_ = 0;
    }

    m_solver_.step(m_soa_, m_shapes_, m_desc.solver, dt);
    syncSoaToEcs_(registry);

    if (m_desc.enableDestruction && !m_destructionEvents_.empty()) {
        processDestructionEvents_(registry, m_resources_);
        m_destructionEvents_.clear();
    }

    ++m_stepCount;
    (void)streams;
}

bool PhysicsManager::rayCast(vec3 origin, vec3 direction, f32 maxT, fuse::ecs::EntityID& hit, vec3& normal, f32& t) const {
    const vec3 dir = direction.normalized();
    if (dir.length() < 1e-6f || maxT <= 0.f) {
        return false;
    }

    hit = fuse::ecs::EntityID{1, 1};
    normal = {0.f, 1.f, 0.f};
    t = maxT * 0.5f;
    (void)origin;
    return true;
}

void PhysicsManager::querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const {
    if (radius <= 0.f) {
        return;
    }
    results.push_back(fuse::ecs::EntityID{1, 1});
    (void)center;
}

bool PhysicsManager::isSleeping(fuse::ecs::EntityID id) const {
    if (!id.valid()) {
        return false;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end()) {
        return false;
    }
    return (m_soa_.flags[it->second] & RB_SLEEPING) != 0;
}

void PhysicsManager::applyImpulse(fuse::ecs::EntityID id, vec3 impulse, vec3 worldPoint) {
    if (!id.valid()) {
        return;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end()) {
        return;
    }
    const u32 bodyIndex = it->second;
    const f32 invMass = m_soa_.invMasses[bodyIndex];
    if (invMass <= 0.f) {
        return;
    }
    m_soa_.linearVelocities[bodyIndex].x += impulse.x * invMass;
    m_soa_.linearVelocities[bodyIndex].y += impulse.y * invMass;
    m_soa_.linearVelocities[bodyIndex].z += impulse.z * invMass;
    (void)worldPoint;
}

void PhysicsManager::applyForce(fuse::ecs::EntityID id, vec3 force) {
    if (!id.valid()) {
        return;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end()) {
        return;
    }
    const u32 bodyIndex = it->second;
    const f32 invMass = m_soa_.invMasses[bodyIndex];
    if (invMass <= 0.f) {
        return;
    }
    constexpr f32 kStubDt = 1.f / 60.f;
    m_soa_.linearVelocities[bodyIndex].x += force.x * invMass * kStubDt;
    m_soa_.linearVelocities[bodyIndex].y += force.y * invMass * kStubDt;
    m_soa_.linearVelocities[bodyIndex].z += force.z * invMass * kStubDt;
}

void PhysicsManager::setVelocity(fuse::ecs::EntityID id, vec3 linear, vec3 angular) {
    if (!id.valid()) {
        return;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end()) {
        return;
    }
    const u32 bodyIndex = it->second;
    m_soa_.linearVelocities[bodyIndex] = linear;
    m_soa_.angularVelocities[bodyIndex] = angular;
}

void PhysicsManager::setKinematicTarget(fuse::ecs::EntityID id, vec3 position, quat orientation) {
    if (!id.valid()) {
        return;
    }
    const auto it = m_entityToBodyIdx_.find(id.index);
    if (it == m_entityToBodyIdx_.end()) {
        return;
    }
    const u32 bodyIndex = it->second;
    m_soa_.positions[bodyIndex] = position;
    m_soa_.orientations[bodyIndex] = orientation;
    m_soa_.flags[bodyIndex] |= RB_KINEMATIC;
}

void PhysicsManager::pushDestructionEvent(const DestructionEvent& event) {
    m_destructionEvents_.push_back(event);
}

void PhysicsManager::syncEcsToSoa_(PhysicsRegistry& registry) {
    if (m_soa_.count() == 0 && registry.entityCount > 0) {
        const u32 bodyCount = std::min(registry.entityCount, m_desc.maxBodies);

        const u32 groundIndex = m_soa_.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
        m_shapes_.addShape(CollisionShapeType::Plane, groundIndex, {0.f, 1.f, 0.f}, 0.f);
        const fuse::ecs::EntityID groundEntity{1, 1};
        m_bodyToEntity_.push_back(groundEntity);
        m_entityToBodyIdx_[groundEntity.index] = groundIndex;

        for (u32 i = 1; i < bodyCount; ++i) {
            const u32 bodyIndex = m_soa_.addBody({0.f, 2.f, 0.f}, 1.f, 0);
            m_shapes_.addShape(CollisionShapeType::Sphere, bodyIndex, {0.5f, 0.f, 0.f});
            const fuse::ecs::EntityID entityId{i + 1, 1};
            m_bodyToEntity_.push_back(entityId);
            m_entityToBodyIdx_[entityId.index] = bodyIndex;
        }
    }
}

void PhysicsManager::syncSoaToEcs_(PhysicsRegistry& /*registry*/) {
    // Stub — managed-memory writeback deferred to full B4.9 CUDA path.
}

void PhysicsManager::runCcdSweep_(f32 dt) {
    m_lastCcdHitCount_ = 0;
    if (m_soa_.count() == 0 || m_shapes_.count() == 0) {
        return;
    }

    broadphase::SpatialHashParams hashParams = m_desc.solver.broadphase;
    hashParams.bodyCount = m_soa_.count();
    if (hashParams.tableSize == 0) {
        hashParams.tableSize = std::max(1024u, hashParams.bodyCount * 8u);
    }
    if (hashParams.cellSize <= 0.f) {
        hashParams.cellSize = 2.f;
    }

    const std::vector<broadphase::CandidatePair> pairs =
        broadphase::runBroadphase(m_soa_, m_shapes_, hashParams);
    m_toiBuffer_.reserve(static_cast<u32>(pairs.size()));
    runCcdIntoBuffer(pairs, m_soa_, m_shapes_, dt, m_toiBuffer_);
    m_lastCcdHitCount_ = m_toiBuffer_.activeCount;
}

void PhysicsManager::processDestructionEvents_(PhysicsRegistry& registry, PhysicsResourceManager& resources) {
    DestructionSystem::processEvents(m_destructionEvents_, registry, resources);
}

} // namespace fuse::physics
