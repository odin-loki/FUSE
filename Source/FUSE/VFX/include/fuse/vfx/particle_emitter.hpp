#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::vfx {

struct ParticleEmitterDesc {
    u32 max_particles = 4096;
    f32 emit_rate = 100.f;
    f32 lifetime_min = 1.f;
    f32 lifetime_max = 3.f;

    math::Vec3 velocity_min{-1.f, -1.f, -1.f};
    math::Vec3 velocity_max{1.f, 5.f, 1.f};
    math::Vec3 position_spread{0.5f, 0.f, 0.5f};
    f32 size_start = 0.1f;
    f32 size_end = 0.f;
    math::Vec3 color_start{1.f, 0.5f, 0.f};
    math::Vec3 color_end{0.2f, 0.1f, 0.f};
    f32 alpha_start = 1.f;
    f32 alpha_end = 0.f;

    math::Vec3 gravity{0.f, -9.81f, 0.f};
    f32 drag = 0.1f;
    bool collide_with_world = false;
    bool affected_by_wind = false;

    u32 material_id = 0;
    bool billboarded = true;
    bool velocity_stretch = false;
    f32 stretch_factor = 2.f;
};

struct ParticleSoA {
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> velocities;
    std::vector<f32> ages;
    std::vector<f32> lifetimes;
    std::vector<f32> sizes;
    std::vector<math::Vec3> colors;
    std::vector<f32> alphas;
    std::vector<u32> alive_flags;
    u32 count = 0;
    u32 capacity = 0;
};

class ParticleEmitter {
public:
    void init(const ParticleEmitterDesc& desc);
    void destroy();

    void set_position(const math::Vec3& world_pos);
    void set_enabled(bool enabled);
    void burst(u32 count);

    void simulate(f32 dt);
    u32 alive_count() const;

    const ParticleEmitterDesc& desc() const { return m_desc; }
    const math::Vec3& position() const { return m_worldPos; }
    bool enabled() const { return m_enabled; }
    bool initialized() const { return m_initialized; }
    const ParticleSoA& particles() const { return m_particles; }

private:
    u32 find_dead_slot_() const;
    void emit_particle_(u64 seed);
    f32 sample_range_(f32 min_value, f32 max_value, u64& seed) const;

    ParticleEmitterDesc m_desc{};
    ParticleSoA m_particles{};
    math::Vec3 m_worldPos{};
    bool m_enabled = true;
    bool m_initialized = false;
    f32 m_emitAccum = 0.f;
    u64 m_frameSeed = 1;
};

} // namespace fuse::vfx
