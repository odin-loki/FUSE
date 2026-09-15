#pragma once

#include <fuse/vfx/particle_emitter.hpp>

#include <vector>

namespace fuse::vfx::particle_soa {

/// Initialize SoA columns and pre-fill the free-list with all slot indices.
void init(ParticleSoA& soa, u32 capacity);

/// Pop a dead slot from `free_slots`, or return `UINT32_MAX` when full.
[[nodiscard]] u32 allocate_slot(ParticleSoA& soa);

/// Return a dead slot index to the free list (caller must clear `alive_flags`).
void recycle_slot(ParticleSoA& soa, u32 slot);

struct BurstEmitResult {
    u32 emitted = 0;
    u64 seed_after = 0;
};

/// Deterministic burst fill: emit up to `count` particles using `seed` for RNG.
[[nodiscard]] BurstEmitResult burst_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc,
                                         const math::Vec3& origin, u32 count, u64 seed);

struct RateEmitResult {
    u32 emitted = 0;
    f32 accum_after = 0.f;
    u64 seed_after = 0;
};

/// Rate-driven emission: advance accumulator by `emit_rate * dt` and emit whole particles.
[[nodiscard]] RateEmitResult accumulate_rate_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc,
                                                  const math::Vec3& origin, f32 dt, f32 emit_accum,
                                                  u64 seed);

struct SimStepResult {
    u32 alive_after = 0;
    std::vector<u32> dead_slots;
};

/// One simulation step: age/kill, velocity integrate, attribute interpolation over live slots.
/// Uses `parallel_for` when the job scheduler has workers; otherwise runs serially.
[[nodiscard]] SimStepResult simulate_step(ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt,
                                          u32 grain_size = 64u);

} // namespace fuse::vfx::particle_soa
