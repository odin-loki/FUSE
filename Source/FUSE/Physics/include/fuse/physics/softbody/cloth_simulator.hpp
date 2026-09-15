#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/softbody/particle_system.hpp>

#include <vector>

namespace fuse::physics {

struct BufferHandleTag {};
using BufferHandle = u32;

struct ClothDesc {
    u32 rows = 0;
    u32 cols = 0;
    f32 particleSpacing = 0.1f;
    f32 stretchCompliance = 0.f;
    f32 bendCompliance = 0.001f;
    u32 pinnedCorners = 0b0011u;
};

/// B4.8 — XPBD cloth scaffold (CPU stub; CUDA kernels deferred).
class ClothSimulator {
public:
    void init(const ClothDesc& desc, vec3 origin);
    void destroy();
    void step(f32 dt, vec3 gravity);
    void applyWind(vec3 windVelocity);

    BufferHandle vertexBuffer() const { return m_vertexBuffer; }
    BufferHandle indexBuffer() const { return m_indexBuffer; }
    u32 indexCount() const { return m_indexCount; }
    u32 particleCount() const { return m_particles.count; }
    bool ready() const { return m_ready; }

private:
    void buildConstraints_();

    bool m_ready = false;
    ClothDesc m_desc{};
    ParticleSoA m_particles{};
    std::vector<ParticleConstraint> m_constraints{};
    BufferHandle m_vertexBuffer = 0;
    BufferHandle m_indexBuffer = 0;
    u32 m_indexCount = 0;
};

} // namespace fuse::physics
