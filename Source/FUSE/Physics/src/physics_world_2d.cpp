#include <fuse/physics/physics_world_2d.hpp>

namespace fuse::physics {

void PhysicsWorld2D::init(const PhysicsPipelineDesc& desc) {
    PhysicsPipelineDesc localDesc = desc;
    localDesc.broadphaseMode = BroadphaseMode::SpatialHash2D;
    m_pipeline.init(localDesc);
}

void PhysicsWorld2D::reset() {
    m_pipeline.reset();
}

u32 PhysicsWorld2D::addCircleBody(float x, float y, f32 radius, f32 invMass) {
    return m_pipeline.addSphereBody({x, y, 0.f}, radius, invMass);
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
}

} // namespace fuse::physics
