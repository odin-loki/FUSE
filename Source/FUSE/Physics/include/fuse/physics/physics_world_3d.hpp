#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

/// 3D physics facade composed by World3D — composes FUSE pipeline, not Box2D.
class PhysicsWorld3D {
public:
    void init(const PhysicsPipelineDesc& desc = {});
    void reset();

    u32 addSphereBody(float x, float y, float z, f32 radius, f32 invMass = 1.f);
    u32 addStaticPlane(vec3 normal, f32 distance);
    void setBodyPosition(u32 bodyIndex, float x, float y, float z);
    void getBodyPosition(u32 bodyIndex, float& x, float& y, float& z) const;

    void step(f32 dt);

    u32 bodyCount() const { return m_pipeline.bodyCount(); }
    u32 contactCount() const { return m_pipeline.contactCount(); }
    const PhysicsPipeline& pipeline() const { return m_pipeline; }

private:
    PhysicsPipeline m_pipeline;
};

} // namespace fuse::physics
