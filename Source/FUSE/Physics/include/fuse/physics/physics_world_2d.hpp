#pragma once

#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

/// 2D physics facade composed by World2D — does not inherit Box2D.
class PhysicsWorld2D {
public:
    void init(const PhysicsPipelineDesc& desc = {});
    void reset();

    u32 addCircleBody(float x, float y, f32 radius, f32 invMass = 1.f);
    void setBodyPosition(u32 bodyIndex, float x, float y);
    void getBodyPosition(u32 bodyIndex, float& x, float& y) const;

    void step(f32 dt);

    u32 bodyCount() const { return m_pipeline.bodyCount(); }
    u32 contactCount() const { return m_pipeline.contactCount(); }
    const PhysicsPipeline& pipeline() const { return m_pipeline; }

private:
    PhysicsPipeline m_pipeline;
};

} // namespace fuse::physics
