#include <fuse/vfx/particle_emitter.hpp>

#include <fuse/vfx/particle_soa_ops.hpp>

namespace fuse::vfx {

void ParticleEmitter::init(const ParticleEmitterDesc& desc) {
    destroy();
    m_desc = desc;
    particle_soa::init(m_particles, desc.max_particles);
    m_enabled = true;
    m_emitAccum = 0.f;
    m_frameSeed = 1;
    m_initialized = true;
}

void ParticleEmitter::destroy() {
    m_particles = ParticleSoA{};
    m_desc = ParticleEmitterDesc{};
    m_worldPos = {};
    m_enabled = true;
    m_emitAccum = 0.f;
    m_frameSeed = 1;
    m_initialized = false;
}

void ParticleEmitter::set_position(const math::Vec3& world_pos) {
    m_worldPos = world_pos;
}

void ParticleEmitter::set_enabled(bool enabled) {
    m_enabled = enabled;
}

void ParticleEmitter::burst(u32 count) {
    if (!m_initialized) {
        return;
    }
    const particle_soa::BurstEmitResult result =
        particle_soa::burst_emit(m_particles, m_desc, m_worldPos, count, m_frameSeed);
    m_frameSeed = result.seed_after;
}

void ParticleEmitter::simulate(f32 dt) {
    if (!m_initialized || !m_enabled || dt <= 0.f) {
        return;
    }

    (void)particle_soa::simulate_step(m_particles, m_desc, dt);

    if (m_desc.emit_rate > 0.f) {
        const particle_soa::RateEmitResult rate =
            particle_soa::accumulate_rate_emit(m_particles, m_desc, m_worldPos, dt, m_emitAccum, m_frameSeed);
        m_emitAccum = rate.accum_after;
        m_frameSeed = rate.seed_after;
    }
}

u32 ParticleEmitter::alive_count() const {
    return m_particles.count;
}

} // namespace fuse::vfx
