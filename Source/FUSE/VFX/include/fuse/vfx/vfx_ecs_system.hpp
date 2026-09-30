#pragma once

// GAP-GAME-LOOP-ECS: particle emitters driven from the ECS registry.
//
// `VfxEmitter` is a plain ECS component carrying the emitter parameters (the POD subset of
// ParticleEmitterDesc: world colliders stay on the ParticleSystem API). Each `update` gives every
// VfxEmitter component a ParticleSystem emitter (firing `burst_on_start` once), follows the entity's
// Transform world translation (+ `offset`), applies `enabled`, releases the emitter of a removed
// component / destroyed entity, then steps ParticleSystem::update(dt) and writes the live particle
// count back into the component. Steady state (no component added / removed) makes no heap
// allocations here (ParticleEmitter::simulate is itself heap-free once its pools exist).

#include <fuse/ecs/entity.hpp>
#include <fuse/handle.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/particle_emitter.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::vfx {

class ParticleSystem;

/// ECS component: a particle emitter attached to the entity.
struct VfxEmitter {
    static constexpr const char* component_name = "VfxEmitter";

    u32 max_particles = 1024;
    f32 emit_rate = 100.f;
    f32 lifetime_min = 1.f;
    f32 lifetime_max = 2.f;
    math::Vec3 velocity_min{-1.f, 0.f, -1.f};
    math::Vec3 velocity_max{1.f, 5.f, 1.f};
    math::Vec3 position_spread{0.25f, 0.f, 0.25f};
    f32 size_start = 0.1f;
    f32 size_end = 0.f;
    math::Vec3 color_start{1.f, 0.5f, 0.f};
    math::Vec3 color_end{0.2f, 0.1f, 0.f};
    f32 alpha_start = 1.f;
    f32 alpha_end = 0.f;
    math::Vec3 gravity{0.f, -9.81f, 0.f};
    f32 drag = 0.1f;
    u32 material_id = 0;
    /// Emitter position relative to the entity's world translation.
    math::Vec3 offset{};
    /// Particles emitted at once when the emitter is created.
    u32 burst_on_start = 0;
    bool enabled = true;

    // Runtime (written by VfxEcsSystem).
    u32 emitter_index = 0xFFFFFFFFu;
    u32 emitter_generation = 0;
    u32 alive_particles = 0;

    [[nodiscard]] ParticleEmitterDesc to_desc() const;
};

struct VfxEcsStats {
    u64 emitters_created = 0;
    u64 emitters_released = 0;
    u32 alive_particles = 0;
};

class VfxEcsSystem {
public:
    /// Non-owning: `particles` (initialised) must outlive the system.
    bool init(ParticleSystem& particles, usize expected_emitters = 64);
    void shutdown();
    [[nodiscard]] bool is_initialized() const { return m_particles != nullptr; }

    void update(ecs::Registry& registry, f32 dt);

    [[nodiscard]] Handle<ParticleEmitter> emitter_of(ecs::EntityID entity) const;
    [[nodiscard]] const VfxEcsStats& stats() const { return m_stats; }

private:
    struct Tracked {
        ecs::EntityID entity = ecs::EntityID::null();
        Handle<ParticleEmitter> emitter = Handle<ParticleEmitter>::invalid();
        u32 stamp = 0;
    };

    static u64 key_of(ecs::EntityID entity) {
        return (static_cast<u64>(entity.generation) << 32) | static_cast<u64>(entity.index);
    }

    ParticleSystem* m_particles = nullptr;
    std::vector<Tracked> m_tracked;
    std::unordered_map<u64, u32> m_index;
    u32 m_stamp = 0;
    VfxEcsStats m_stats{};
};

} // namespace fuse::vfx
