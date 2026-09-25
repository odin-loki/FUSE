#include <fuse/vfx/particle_emitter.hpp>

#include <fuse/vfx/particle_soa_ops.hpp>

#include <cmath>

namespace fuse::vfx {

void ParticleEmitter::init(const ParticleEmitterDesc& desc) {
    destroy();
    m_desc = desc;
    particle_soa::init(m_particles, desc.max_particles);
    m_enabled = true;
    m_emitAccum = 0.0;
    m_totalEmitted = 0;
    m_frameSeed = 1;
    m_initialized = true;
}

void ParticleEmitter::destroy() {
    m_particles = ParticleSoA{};
    m_desc = ParticleEmitterDesc{};
    m_worldPos = {};
    m_enabled = true;
    m_gpuSimulation = false;
    m_emitAccum = 0.0;
    m_totalEmitted = 0;
    m_frameSeed = 1;
    m_initialized = false;
}

void ParticleEmitter::set_position(const math::Vec3& world_pos) {
    m_worldPos = world_pos;
}

void ParticleEmitter::set_enabled(bool enabled) {
    m_enabled = enabled;
}

u32 ParticleEmitter::burst(u32 count) {
    if (!m_initialized) {
        return 0u;
    }
    const particle_soa::BurstEmitResult result =
        particle_soa::burst_emit(m_particles, m_desc, m_worldPos, count, m_frameSeed);
    m_frameSeed = result.seed_after;
    m_totalEmitted += result.emitted;
    return result.emitted;
}

void ParticleEmitter::simulate(f32 dt) {
    if (!m_initialized || !m_enabled || dt <= 0.f) {
        return;
    }

    if (m_gpuSimulation) {
        (void)particle_soa::simulate_step_on(kernel::Backend::Cuda, m_particles, m_desc, dt, m_deadSlotScratch);
    } else {
        (void)particle_soa::simulate_step(m_particles, m_desc, dt, m_deadSlotScratch);
    }

    if (m_desc.emit_rate > 0.f && m_particles.capacity > 0u) {
        // Double-precision accumulator: an f32 fraction drifts by ~1 ulp per frame, which loses or
        // gains whole particles over long runs (e.g. 499 instead of 500 after 5 s at 100/s).
        m_emitAccum += static_cast<f64>(m_desc.emit_rate) * static_cast<f64>(dt);
        const f64 whole = std::floor(m_emitAccum);
        const u32 wanted = whole >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<u32>(whole);
        if (wanted > 0u) {
            const particle_soa::BurstEmitResult emitted =
                particle_soa::burst_emit(m_particles, m_desc, m_worldPos, wanted, m_frameSeed);
            m_frameSeed = emitted.seed_after;
            m_totalEmitted += emitted.emitted;
            // At capacity the backlog is dropped (matches accumulate_rate_emit).
            m_emitAccum = emitted.emitted < wanted ? 0.0 : m_emitAccum - whole;
        }
    }
}

u32 ParticleEmitter::alive_count() const {
    return m_particles.count;
}

} // namespace fuse::vfx
