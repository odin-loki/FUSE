#include <fuse/physics/softbody/cloth_simulator.hpp>

namespace fuse::physics {

void ClothSimulator::init(const ClothDesc& desc, vec3 origin) {
    destroy();
    if (desc.rows < 2 || desc.cols < 2) {
        return;
    }

    m_desc = desc;
    const u32 particleCount = desc.rows * desc.cols;
    m_particles = ParticleSoA::allocate(particleCount);
    m_particles.count = particleCount;

    for (u32 row = 0; row < desc.rows; ++row) {
        for (u32 col = 0; col < desc.cols; ++col) {
            const u32 index = row * desc.cols + col;
            m_particles.positions[index] = {
                origin.x + static_cast<f32>(col) * desc.particleSpacing,
                origin.y,
                origin.z + static_cast<f32>(row) * desc.particleSpacing,
            };
            m_particles.prevPositions[index] = m_particles.positions[index];
            m_particles.velocities[index] = {};
            m_particles.invMasses[index] = 1.f;
        }
    }

    const u32 cornerMask = desc.pinnedCorners;
    const u32 lastRow = desc.rows - 1;
    const u32 lastCol = desc.cols - 1;
    if (cornerMask & 0b0001u) {
        m_particles.invMasses[0] = 0.f;
    }
    if (cornerMask & 0b0010u) {
        m_particles.invMasses[lastCol] = 0.f;
    }
    if (cornerMask & 0b0100u) {
        m_particles.invMasses[lastRow * desc.cols] = 0.f;
    }
    if (cornerMask & 0b1000u) {
        m_particles.invMasses[lastRow * desc.cols + lastCol] = 0.f;
    }

    buildConstraints_();
    m_vertexBuffer = 1;
    m_indexBuffer = 2;
    m_indexCount = (desc.rows - 1) * (desc.cols - 1) * 6;
    m_ready = true;
}

void ClothSimulator::destroy() {
    ParticleSoA::free(m_particles);
    m_constraints.clear();
    m_vertexBuffer = 0;
    m_indexBuffer = 0;
    m_indexCount = 0;
    m_ready = false;
}

void ClothSimulator::step(f32 dt, vec3 gravity) {
    if (!m_ready) {
        return;
    }

    for (u32 i = 0; i < m_particles.count; ++i) {
        if (m_particles.invMasses[i] == 0.f) {
            continue;
        }

        m_particles.velocities[i].x += gravity.x * dt;
        m_particles.velocities[i].y += gravity.y * dt;
        m_particles.velocities[i].z += gravity.z * dt;
        m_particles.prevPositions[i] = m_particles.positions[i];
        m_particles.positions[i].x += m_particles.velocities[i].x * dt;
        m_particles.positions[i].y += m_particles.velocities[i].y * dt;
        m_particles.positions[i].z += m_particles.velocities[i].z * dt;
    }

    for (const ParticleConstraint& constraint : m_constraints) {
        vec3& pa = m_particles.positions[constraint.a];
        vec3& pb = m_particles.positions[constraint.b];
        vec3 diff = pa - pb;
        const f32 dist = diff.length();
        if (dist < 1e-6f) {
            continue;
        }

        const f32 wSum = m_particles.invMasses[constraint.a] + m_particles.invMasses[constraint.b];
        if (wSum <= 0.f) {
            continue;
        }

        const f32 alpha = constraint.compliance / (dt * dt);
        const f32 correction = -(dist - constraint.restLength) / (wSum + alpha);
        const vec3 n = diff.normalized();
        const vec3 delta = n * correction;

        pa = pa + delta * m_particles.invMasses[constraint.a];
        pb = pb - delta * m_particles.invMasses[constraint.b];
    }
}

void ClothSimulator::applyWind(vec3 windVelocity) {
    if (!m_ready) {
        return;
    }

    for (u32 i = 0; i < m_particles.count; ++i) {
        if (m_particles.invMasses[i] == 0.f) {
            continue;
        }
        m_particles.velocities[i].x += windVelocity.x * 0.01f;
        m_particles.velocities[i].y += windVelocity.y * 0.01f;
        m_particles.velocities[i].z += windVelocity.z * 0.01f;
    }
}

void ClothSimulator::buildConstraints_() {
    m_constraints.clear();
    for (u32 row = 0; row < m_desc.rows; ++row) {
        for (u32 col = 0; col < m_desc.cols; ++col) {
            const u32 index = row * m_desc.cols + col;
            if (col + 1 < m_desc.cols) {
                m_constraints.push_back(
                    {index, index + 1, m_desc.particleSpacing, m_desc.stretchCompliance, 1.f});
            }
            if (row + 1 < m_desc.rows) {
                m_constraints.push_back(
                    {index, index + m_desc.cols, m_desc.particleSpacing, m_desc.stretchCompliance, 1.f});
            }
        }
    }
}

} // namespace fuse::physics
