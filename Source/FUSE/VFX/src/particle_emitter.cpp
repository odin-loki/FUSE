#include <fuse/vfx/particle_emitter.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::vfx {

namespace {

u64 mix_seed(u64 seed) {
    seed ^= seed >> 33U;
    seed *= 0xff51afd7ed558ccdULL;
    seed ^= seed >> 33U;
    seed *= 0xc4ceb9fe1a85ec53ULL;
    seed ^= seed >> 33U;
    return seed;
}

f32 unit_float(u64& seed) {
    seed = mix_seed(seed + 0x9e3779b97f4a7c15ULL);
    return static_cast<f32>(seed & 0xFFFFFFU) / static_cast<f32>(0xFFFFFFU);
}

} // namespace

void ParticleEmitter::init(const ParticleEmitterDesc& desc) {
    destroy();
    m_desc = desc;
    m_particles.capacity = desc.max_particles;
    m_particles.positions.assign(desc.max_particles, {});
    m_particles.velocities.assign(desc.max_particles, {});
    m_particles.ages.assign(desc.max_particles, 0.f);
    m_particles.lifetimes.assign(desc.max_particles, 0.f);
    m_particles.sizes.assign(desc.max_particles, 0.f);
    m_particles.colors.assign(desc.max_particles, {});
    m_particles.alphas.assign(desc.max_particles, 0.f);
    m_particles.alive_flags.assign(desc.max_particles, 0U);
    m_particles.count = 0;
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
    for (u32 i = 0; i < count; ++i) {
        if (find_dead_slot_() == UINT32_MAX) {
            break;
        }
        emit_particle_(m_frameSeed++);
    }
}

void ParticleEmitter::simulate(f32 dt) {
    if (!m_initialized || !m_enabled || dt <= 0.f) {
        return;
    }

    u32 alive = 0;
    for (u32 i = 0; i < m_particles.capacity; ++i) {
        if (m_particles.alive_flags[i] == 0U) {
            continue;
        }

        m_particles.ages[i] += dt / std::max(m_particles.lifetimes[i], 1e-4f);
        if (m_particles.ages[i] >= 1.f) {
            m_particles.alive_flags[i] = 0U;
            continue;
        }

        math::Vec3& velocity = m_particles.velocities[i];
        velocity = velocity + m_desc.gravity * dt;
        velocity = velocity * (1.f - m_desc.drag * dt);

        math::Vec3& position = m_particles.positions[i];
        position = position + velocity * dt;

        const f32 t = m_particles.ages[i];
        m_particles.sizes[i] = m_desc.size_start + (m_desc.size_end - m_desc.size_start) * t;
        m_particles.colors[i] = m_desc.color_start + (m_desc.color_end - m_desc.color_start) * t;
        m_particles.alphas[i] = m_desc.alpha_start + (m_desc.alpha_end - m_desc.alpha_start) * t;
        ++alive;
    }
    m_particles.count = alive;

    if (m_desc.emit_rate > 0.f) {
        m_emitAccum += m_desc.emit_rate * dt;
        while (m_emitAccum >= 1.f) {
            if (find_dead_slot_() == UINT32_MAX) {
                m_emitAccum = 0.f;
                break;
            }
            emit_particle_(m_frameSeed++);
            m_emitAccum -= 1.f;
        }
    }
}

u32 ParticleEmitter::alive_count() const {
    return m_particles.count;
}

u32 ParticleEmitter::find_dead_slot_() const {
    for (u32 i = 0; i < m_particles.capacity; ++i) {
        if (m_particles.alive_flags[i] == 0U) {
            return i;
        }
    }
    return UINT32_MAX;
}

void ParticleEmitter::emit_particle_(u64 seed) {
    const u32 slot = find_dead_slot_();
    if (slot == UINT32_MAX) {
        return;
    }

    const f32 spread_x = sample_range_(-m_desc.position_spread.x, m_desc.position_spread.x, seed);
    const f32 spread_y = sample_range_(-m_desc.position_spread.y, m_desc.position_spread.y, seed);
    const f32 spread_z = sample_range_(-m_desc.position_spread.z, m_desc.position_spread.z, seed);

    m_particles.positions[slot] = m_worldPos + math::Vec3{spread_x, spread_y, spread_z};
    m_particles.velocities[slot] = {
        sample_range_(m_desc.velocity_min.x, m_desc.velocity_max.x, seed),
        sample_range_(m_desc.velocity_min.y, m_desc.velocity_max.y, seed),
        sample_range_(m_desc.velocity_min.z, m_desc.velocity_max.z, seed),
    };
    m_particles.lifetimes[slot] = sample_range_(m_desc.lifetime_min, m_desc.lifetime_max, seed);
    m_particles.ages[slot] = 0.f;
    m_particles.sizes[slot] = m_desc.size_start;
    m_particles.colors[slot] = m_desc.color_start;
    m_particles.alphas[slot] = m_desc.alpha_start;
    m_particles.alive_flags[slot] = 1U;
    ++m_particles.count;
}

f32 ParticleEmitter::sample_range_(f32 min_value, f32 max_value, u64& seed) const {
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    return min_value + unit_float(seed) * (max_value - min_value);
}

} // namespace fuse::vfx
