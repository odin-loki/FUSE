#include <fuse/vfx/particle_soa_ops.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/vfx/particle_sim_kernel.hpp>
#include <fuse/vfx/particle_system.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fuse::vfx {

#if defined(FUSE_HAS_CUDA)
/// kernels/particle_sim.cu: stages the SoA on the device and runs the same update + compaction bodies.
/// `params` reference host memory; `dead_slots` must hold `capacity` entries.
bool launchParticleSimCuda(const particle_sim_kernel::Params& params, u32 capacity, void* stream);
#endif

bool particle_cuda_kernel_available() {
#if defined(FUSE_HAS_CUDA)
    // kernels/particle_sim.cu is compiled whenever FUSE_HAS_CUDA is; a device must also be usable.
    return kernel::backend_available(kernel::Backend::Cuda);
#else
    return false;
#endif
}

} // namespace fuse::vfx

namespace fuse::vfx::particle_soa {

namespace psk = particle_sim_kernel;

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

f32 sample_range(f32 min_value, f32 max_value, u64& seed) {
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    return min_value + unit_float(seed) * (max_value - min_value);
}

void fill_slot(ParticleSoA& soa, u32 slot, const ParticleEmitterDesc& desc, const math::Vec3& origin,
               u64& seed) {
    const f32 spread_x = sample_range(-desc.position_spread.x, desc.position_spread.x, seed);
    const f32 spread_y = sample_range(-desc.position_spread.y, desc.position_spread.y, seed);
    const f32 spread_z = sample_range(-desc.position_spread.z, desc.position_spread.z, seed);

    soa.positions[slot] = origin + math::Vec3{spread_x, spread_y, spread_z};
    soa.velocities[slot] = {
        sample_range(desc.velocity_min.x, desc.velocity_max.x, seed),
        sample_range(desc.velocity_min.y, desc.velocity_max.y, seed),
        sample_range(desc.velocity_min.z, desc.velocity_max.z, seed),
    };
    soa.lifetimes[slot] = sample_range(desc.lifetime_min, desc.lifetime_max, seed);
    soa.ages[slot] = 0.f;
    soa.sizes[slot] = desc.size_start;
    soa.colors[slot] = desc.color_start;
    soa.alphas[slot] = desc.alpha_start;
    soa.alive_flags[slot] = 1U;
    ++soa.count;
}

struct SimCounts {
    u32 alive = 0;
    u32 culled = 0;
    bool ok = false;
};

/// Counters + per-workgroup dead counts, zeroed for the next update (allocates only when the
/// capacity changed since `init`).
void reset_sim_scratch(ParticleSoA& soa) {
    const usize needed = psk::kCounterCount + psk::group_count(soa.capacity);
    if (soa.sim_scratch.size() != needed) {
        soa.sim_scratch.assign(needed, 0u);
    } else {
        std::fill(soa.sim_scratch.begin(), soa.sim_scratch.end(), 0u);
    }
}

psk::Params make_sim_params(ParticleSoA& soa, const ParticleEmitterDesc* desc, f32 dt, bool integrate,
                            std::vector<u32>& dead_slots) {
    const u32 n = soa.capacity;
    psk::Params p{};
    p.positions = {soa.positions.data(), n};
    p.velocities = {soa.velocities.data(), n};
    p.ages = {soa.ages.data(), n};
    p.lifetimes = {soa.lifetimes.data(), n};
    p.sizes = {soa.sizes.data(), n};
    p.colors = {soa.colors.data(), n};
    p.alphas = {soa.alphas.data(), n};
    p.alive_flags = {soa.alive_flags.data(), n};
    if (integrate && desc != nullptr && desc->collide_with_world && !desc->colliders.empty()) {
        p.colliders = {desc->colliders.data(), static_cast<u32>(desc->colliders.size())};
    }
    p.counters = {soa.sim_scratch.data(), psk::kCounterCount};
    p.group_dead = {soa.sim_scratch.data() + psk::kCounterCount, psk::group_count(n)};
    p.dead_slots = {dead_slots.data(), static_cast<u32>(dead_slots.size())};
    p.sim = psk::make_sim(desc, dt, integrate);
    return p;
}

/// One update + compaction on `backend`. Writes the dead slots (descending) to `dead_slots`,
/// updates `soa.count` and returns them to the free list.
SimCounts run_sim(kernel::Backend backend, ParticleSoA& soa, const ParticleEmitterDesc* desc, f32 dt,
                  bool integrate, u32 grain_size, std::vector<u32>& dead_slots) {
    SimCounts counts{};
    reset_sim_scratch(soa);
    if (dead_slots.capacity() < soa.capacity) {
        dead_slots.reserve(soa.capacity); // once per emitter: every later step stays heap-free
    }

#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) && particle_cuda_kernel_available()) {
        dead_slots.resize(soa.capacity);
        counts.ok = launchParticleSimCuda(make_sim_params(soa, desc, dt, integrate, dead_slots), soa.capacity,
                                          nullptr);
        counts.culled = counts.ok ? soa.sim_scratch[psk::kCounterCulled] : 0u;
        counts.alive = counts.ok ? soa.sim_scratch[psk::kCounterAlive] : soa.count;
        dead_slots.resize(counts.culled);
    } else
#endif
    {
        // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
        // (CpuParallel) and records the requested vs executed backend.
        dead_slots.clear();
        psk::Params params = make_sim_params(soa, desc, dt, integrate, dead_slots);
        kernel::LaunchOptions options{};
        options.grain_workgroups = kernel::div_up(std::max(grain_size, 1u), psk::kGroupSize);
        counts.ok = kernel::launch(backend, psk::update_launch(soa.capacity), psk::UpdateKernel{}, params, options).ok;
        counts.culled = counts.ok ? soa.sim_scratch[psk::kCounterCulled] : 0u;
        counts.alive = counts.ok ? soa.sim_scratch[psk::kCounterAlive] : soa.count;
        if (counts.culled > 0u) {
            dead_slots.resize(counts.culled);
            params.dead_slots = {dead_slots.data(), counts.culled};
            counts.ok = kernel::launch(backend, psk::compact_launch(soa.capacity), psk::CompactKernel{}, params,
                                       options)
                            .ok;
        }
    }

    if (counts.ok) {
        soa.count = counts.alive;
        recycle_slots(soa, dead_slots);
    }
    return counts;
}

} // namespace

void init(ParticleSoA& soa, u32 capacity) {
    soa.capacity = capacity;
    soa.positions.assign(capacity, {});
    soa.velocities.assign(capacity, {});
    soa.ages.assign(capacity, 0.f);
    soa.lifetimes.assign(capacity, 0.f);
    soa.sizes.assign(capacity, 0.f);
    soa.colors.assign(capacity, {});
    soa.alphas.assign(capacity, 0.f);
    soa.alive_flags.assign(capacity, 0U);
    soa.sim_scratch.assign(psk::kCounterCount + psk::group_count(capacity), 0U);
    soa.free_slots.clear();
    soa.free_slots.reserve(capacity);
    for (u32 i = capacity; i > 0u; --i) {
        soa.free_slots.push_back(i - 1u);
    }
    soa.count = 0;
}

u32 free_slot_count(const ParticleSoA& soa) {
    return static_cast<u32>(soa.free_slots.size());
}

u32 clamp_burst_count(const ParticleSoA& soa, u32 requested) {
    const u32 free = free_slot_count(soa);
    return requested < free ? requested : free;
}

bool is_at_capacity(const ParticleSoA& soa) {
    return free_slot_count(soa) == 0u;
}

bool has_live_particles(const ParticleSoA& soa) {
    return soa.count > 0u;
}

u32 count_live_flags(const ParticleSoA& soa) {
    u32 alive = 0;
    for (u32 index = 0; index < soa.capacity; ++index) {
        if (soa.alive_flags[index] != 0U) {
            ++alive;
        }
    }
    return alive;
}

bool can_burst_emit(const ParticleSoA& soa, u32 count) {
    return count > 0u && soa.capacity > 0u && clamp_burst_count(soa, count) > 0u;
}

BurstEmitPreflight preflight_burst_emit(const ParticleSoA& soa, u32 requested) {
    BurstEmitPreflight preflight{};
    preflight.requested = requested;
    preflight.remaining_free = free_slot_count(soa);
    preflight.at_capacity = is_at_capacity(soa);
    preflight.allowed = clamp_burst_count(soa, requested);
    preflight.would_clamp = requested > 0u && soa.capacity > 0u && preflight.allowed < requested;
    preflight.can_emit = preflight.allowed > 0u;
    return preflight;
}

u32 sync_alive_count(ParticleSoA& soa) {
    const u32 alive = count_live_flags(soa);
    soa.count = alive;
    return alive;
}

u32 allocate_slot(ParticleSoA& soa) {
    if (soa.free_slots.empty()) {
        return UINT32_MAX;
    }
    const u32 slot = soa.free_slots.back();
    soa.free_slots.pop_back();
    return slot;
}

void recycle_slot(ParticleSoA& soa, u32 slot) {
    soa.free_slots.push_back(slot);
}

void recycle_slots(ParticleSoA& soa, const std::vector<u32>& slots) {
    soa.free_slots.insert(soa.free_slots.end(), slots.begin(), slots.end());
}

BurstEmitResult burst_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc, const math::Vec3& origin,
                           u32 count, u64 seed) {
    BurstEmitResult result{};
    result.requested = count;
    result.seed_after = seed;
    const BurstEmitPreflight preflight = preflight_burst_emit(soa, count);
    if (!preflight.can_emit) {
        result.clamped = preflight.would_clamp;
        return result;
    }
    for (u32 i = 0; i < preflight.allowed; ++i) {
        const u32 slot = allocate_slot(soa);
        if (slot == UINT32_MAX) {
            break;
        }
        fill_slot(soa, slot, desc, origin, result.seed_after);
        ++result.emitted;
    }
    result.clamped = preflight.would_clamp || (result.requested > 0u && result.emitted < result.requested);
    return result;
}

bool should_skip_lifetime_cull(const ParticleSoA& soa, f32 dt) {
    return dt <= 0.f || soa.capacity == 0u || !has_live_particles(soa);
}

kernel::Backend cpu_simulation_backend(u32 capacity) {
    return capacity < kParallelSlotThreshold ? kernel::Backend::CpuReference : kernel::Backend::CpuParallel;
}

LifetimeCullResult lifetime_cull_on(kernel::Backend backend, ParticleSoA& soa, f32 dt, u32 grain_size) {
    LifetimeCullResult result{};
    if (should_skip_lifetime_cull(soa, dt)) {
        result.alive_after = soa.count;
        return result;
    }
    const SimCounts counts = run_sim(backend, soa, nullptr, dt, false, grain_size, result.dead_slots);
    result.aged = counts.alive + counts.culled;
    result.culled = counts.culled;
    result.alive_after = soa.count;
    return result;
}

LifetimeCullResult lifetime_cull(ParticleSoA& soa, f32 dt, u32 grain_size) {
    return lifetime_cull_on(cpu_simulation_backend(soa.capacity), soa, dt, grain_size);
}

bool should_skip_rate_emit(const ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt) {
    return desc.emit_rate <= 0.f || dt <= 0.f || soa.capacity == 0u;
}

RateEmitPreflight preflight_rate_emit(const ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt) {
    RateEmitPreflight preflight{};
    preflight.remaining_free = free_slot_count(soa);
    preflight.at_capacity = is_at_capacity(soa);
    preflight.skipped = should_skip_rate_emit(soa, desc, dt);
    return preflight;
}

RateEmitResult accumulate_rate_emit(ParticleSoA& soa, const ParticleEmitterDesc& desc, const math::Vec3& origin,
                                    f32 dt, f32 emit_accum, u64 seed) {
    RateEmitResult result{};
    result.accum_after = emit_accum;
    result.seed_after = seed;
    if (should_skip_rate_emit(soa, desc, dt)) {
        return result;
    }

    result.accum_after += desc.emit_rate * dt;
    while (result.accum_after >= 1.f) {
        const u32 slot = allocate_slot(soa);
        if (slot == UINT32_MAX) {
            result.accum_after = 0.f;
            result.at_capacity = true;
            break;
        }
        fill_slot(soa, slot, desc, origin, result.seed_after);
        ++result.emitted;
        result.accum_after -= 1.f;
    }
    return result;
}

bool should_skip_simulate_step(const ParticleSoA& soa, f32 dt) {
    return dt <= 0.f || soa.capacity == 0u || !has_live_particles(soa);
}

SimStepPreflight preflight_simulate_step(const ParticleSoA& soa, f32 dt) {
    SimStepPreflight preflight{};
    preflight.dt = dt;
    preflight.live_input = soa.count;
    preflight.skipped = should_skip_simulate_step(soa, dt);
    return preflight;
}

SimStepResult simulate_step_on(kernel::Backend backend, ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt,
                               std::vector<u32>& dead_slots_scratch, u32 grain_size) {
    SimStepResult result{};
    if (preflight_simulate_step(soa, dt).skipped) {
        result.skipped = true;
        result.alive_after = soa.count;
        return result;
    }
    const SimCounts counts = run_sim(backend, soa, &desc, dt, true, grain_size, dead_slots_scratch);
    result.integrated = counts.alive;
    result.culled = counts.culled;
    result.alive_after = soa.count;
    return result;
}

SimStepResult simulate_step(ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt, u32 grain_size) {
    std::vector<u32> dead_slots;
    SimStepResult result = simulate_step_on(cpu_simulation_backend(soa.capacity), soa, desc, dt, dead_slots, grain_size);
    result.dead_slots = std::move(dead_slots);
    return result;
}

SimStepResult simulate_step(ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt,
                            std::vector<u32>& dead_slots_scratch, u32 grain_size) {
    return simulate_step_on(cpu_simulation_backend(soa.capacity), soa, desc, dt, dead_slots_scratch, grain_size);
}

} // namespace fuse::vfx::particle_soa
