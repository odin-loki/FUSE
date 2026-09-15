#include <fuse/physics/physics_world_3d.hpp>

namespace fuse::physics {

void PhysicsWorld3D::init(const PhysicsPipelineDesc& desc) {
    m_pipeline.init(desc);
}

void PhysicsWorld3D::reset() {
    m_pipeline.reset();
}

u32 PhysicsWorld3D::addSphereBody(float x, float y, float z, f32 radius, f32 invMass) {
    return m_pipeline.addSphereBody({x, y, z}, radius, invMass);
}

u32 PhysicsWorld3D::addStaticPlane(vec3 normal, f32 distance) {
    return m_pipeline.addStaticPlane(normal, distance);
}

void PhysicsWorld3D::setBodyPosition(u32 bodyIndex, float x, float y, float z) {
    if (bodyIndex >= m_pipeline.bodies().count()) {
        return;
    }
    m_pipeline.bodies().positions.at(bodyIndex) = {x, y, z};
}

void PhysicsWorld3D::getBodyPosition(u32 bodyIndex, float& x, float& y, float& z) const {
    if (bodyIndex >= m_pipeline.bodies().count()) {
        x = 0.f;
        y = 0.f;
        z = 0.f;
        return;
    }
    const vec3& position = m_pipeline.bodies().positions.at(bodyIndex);
    x = position.x;
    y = position.y;
    z = position.z;
}

void PhysicsWorld3D::step(f32 dt) {
    m_pipeline.step(dt);
}

} // namespace fuse::physics
