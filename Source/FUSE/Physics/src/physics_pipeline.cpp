#include <fuse/physics/physics_pipeline.hpp>

namespace fuse::physics {

void PhysicsPipeline::init(const PhysicsPipelineDesc& desc) {
    m_desc = desc;
    m_bodies.reserve(desc.maxBodies);
    m_contactBuffer.reserve(desc.maxPairs > 0 ? desc.maxPairs : 16384u);
    m_pairBuffer.reserve(desc.maxPairs > 0 ? desc.maxPairs : 16384u);
    if (desc.maxPairs > 0) {
        m_pairBuffer.setMaxCapacity(desc.maxPairs);
    }
    m_hashParams.cellSize = desc.cellSize;
    m_hashParams.tableSize = desc.maxBodies > 0 ? desc.maxBodies * 2u : 1024u;
    m_initialized = true;
}

void PhysicsPipeline::reset() {
    m_bodies.clear();
    m_shapes.clear();
    m_pairBuffer.clear();
    m_candidatePairs.clear();
    m_contactBuffer.clear();
    m_contacts.clear();
    m_lastDt = 0.f;
}

u32 PhysicsPipeline::addSphereBody(vec3 position, f32 radius, f32 invMass, u32 flags) {
    const u32 bodyIndex = m_bodies.addBody(position, invMass, flags);
    m_shapes.addShape(CollisionShapeType::Sphere, bodyIndex, {radius, 0.f, 0.f});
    return bodyIndex;
}

u32 PhysicsPipeline::addStaticPlane(vec3 normal, f32 distance) {
    const u32 bodyIndex = m_bodies.addBody({}, 0.f, RB_STATIC);
    m_shapes.addShape(CollisionShapeType::Plane, bodyIndex, normal, distance);
    return bodyIndex;
}

void PhysicsPipeline::integrateStub(f32 dt) {
    const vec3 gravity{0.f, -9.81f, 0.f};
    for (u32 i = 0; i < m_bodies.count(); ++i) {
        if ((m_bodies.flags[i] & RB_STATIC) != 0 || (m_bodies.flags[i] & RB_KINEMATIC) != 0) {
            continue;
        }
        if ((m_bodies.flags[i] & RB_NO_GRAVITY) == 0) {
            m_bodies.linearVelocities[i] = m_bodies.linearVelocities[i] + gravity * dt;
        }
        m_bodies.positions[i] = m_bodies.positions[i] + m_bodies.linearVelocities[i] * dt;
        m_bodies.predictedPositions[i] = m_bodies.positions[i];
    }
}

void PhysicsPipeline::step(f32 dt) {
    if (!m_initialized) {
        init({});
    }

    m_lastDt = dt;
    m_hashParams.bodyCount = m_bodies.count();

    integrateStub(dt);

    if (m_desc.broadphaseMode == BroadphaseMode::SpatialHash2D) {
        broadphase::runBroadphase2DIntoBuffer(m_bodies, m_shapes, m_hashParams, m_pairBuffer);
    } else {
        broadphase::runBroadphaseIntoBuffer(m_bodies, m_shapes, m_hashParams, m_pairBuffer);
    }
    m_candidatePairs = m_pairBuffer.toVector();

    narrowphase::runNarrowphaseIntoBuffer(m_candidatePairs, m_bodies, m_shapes, m_contactBuffer);
    m_contacts = m_contactBuffer.toVector();
}

u32 PhysicsPipeline::contactCount() const {
    return m_contactBuffer.activeCount;
}

} // namespace fuse::physics
