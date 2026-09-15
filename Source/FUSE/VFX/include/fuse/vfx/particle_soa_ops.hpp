#pragma once

#include <fuse/vfx/particle_emitter.hpp>

#include <vector>

namespace fuse::vfx::particle_soa {

/// Initialize SoA columns and pre-fill the free-list with all slot indices.
void init(ParticleSoA& soa, u32 capacity);

/// Remaining emission capacity (size of `free_slots`).
[[nodiscard]] u32 free_slot_count(const ParticleSoA& soa);

/// Clamp a burst request to remaining free slots (zero when at capacity).
[[nodiscard]] u32 clamp_burst_count(const ParticleSoA& soa, u32 requested);

/// True when no free slots remain (`free_slot_count == 0`).
[[nodiscard]] bool is_at_capacity(const ParticleSoA& soa);

/// True when at least one live particle is tracked in `soa.count`.
[[nodiscard]] bool has_live_particles(const ParticleSoA& soa);

/// Recount live slots from `alive_flags` and write `soa.count`.
[[nodiscard]] u32 sync_alive_count(ParticleSoA& soa);

/// Pop a dead slot from `free_slots`, or return `UINT32_MAX` when full.
[[nodiscard]] u32 allocate_slot(ParticleSoA& soa);

/// Return a dead slot index to the free list (caller must clear `alive_flags`).
void recycle_slot(ParticleSoA& soa, u32 slot);

/// Return multiple dead slot indices to the free list (caller must clear `alive_flags`).
void recycle_slots(ParticleSoA& soa, const std::vector<u32>& slots);

struct BurstEmitResult {
    u32 requested = 0;
    u32 emitted = 0;
    bool clamped = false;
    u64 seed_after = 0;
};

/// Deterministic burst fill: emit up to `count` particles using `seed` for RNG.
[[nodiscard]] BurstEmitResult burst_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc,
                                         const math::Vec3& origin, u32 count, u64 seed);

struct RateEmitResult {
    u32 emitted = 0;
    f32 accum_after = 0.f;
    bool at_capacity = false;
    u64 seed_after = 0;
};

/// Rate-driven emission: advance accumulator by `emit_rate * dt` and emit whole particles.
[[nodiscard]] RateEmitResult accumulate_rate_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc,
                                                  const math::Vec3& origin, f32 dt, f32 emit_accum,
                                                  u64 seed);

struct LifetimeCullResult {
    u32 aged = 0;
    u32 culled = 0;
    u32 alive_after = 0;
    std::vector<u32> dead_slots;
};

/// Age live slots by `dt` and recycle expired particles without integrating motion or attributes.
/// Uses `parallel_for` when the job scheduler has workers; otherwise runs serially.
[[nodiscard]] LifetimeCullResult lifetime_cull(ParticleSoA& soa, f32 dt, u32 grain_size = 64u);

struct SimStepResult {
    u32 integrated = 0;
    u32 culled = 0;
    u32 alive_after = 0;
    std::vector<u32> dead_slots;
};

/// One simulation step: age/kill, velocity integrate, attribute interpolation over live slots.
/// Uses `parallel_for` when the job scheduler has workers; otherwise runs serially.
[[nodiscard]] SimStepResult simulate_step(ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt,
                                          u32 grain_size = 64u);

} // namespace fuse::vfx::particle_soa
