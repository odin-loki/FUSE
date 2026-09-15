#include <fuse/vfx/particle_soa_ops.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace fuse::vfx::particle_soa {

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

bool advance_age_and_cull_slot(ParticleSoA& soa, u32 index, f32 dt) {
    soa.ages[index] += dt / std::max(soa.lifetimes[index], 1e-4f);
    if (soa.ages[index] >= 1.f) {
        soa.alive_flags[index] = 0U;
        return true;
    }
    return false;
}

void integrate_slot(ParticleSoA& soa, u32 index, const ParticleEmitterDesc& desc, f32 dt,
                    std::vector<u32>& dead_slots, std::mutex& dead_mutex, std::atomic<u32>& alive_count,
                    std::atomic<u32>& integrated_count, std::atomic<u32>& culled_count) {
    if (advance_age_and_cull_slot(soa, index, dt)) {
        std::lock_guard<std::mutex> guard(dead_mutex);
        dead_slots.push_back(index);
        culled_count.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    math::Vec3& velocity = soa.velocities[index];
    velocity = velocity + desc.gravity * dt;
    velocity = velocity * (1.f - desc.drag * dt);

    math::Vec3& position = soa.positions[index];
    position = position + velocity * dt;

    const f32 t = soa.ages[index];
    soa.sizes[index] = desc.size_start + (desc.size_end - desc.size_start) * t;
    soa.colors[index] = desc.color_start + (desc.color_end - desc.color_start) * t;
    soa.alphas[index] = desc.alpha_start + (desc.alpha_end - desc.alpha_start) * t;
    alive_count.fetch_add(1U, std::memory_order_relaxed);
    integrated_count.fetch_add(1U, std::memory_order_relaxed);
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

LifetimeCullResult lifetime_cull(ParticleSoA& soa, f32 dt, u32 grain_size) {
    LifetimeCullResult result{};
    if (should_skip_lifetime_cull(soa, dt)) {
        result.alive_after = soa.count;
        return result;
    }

    result.dead_slots.reserve(soa.capacity / 8u + 1u);
    std::mutex dead_mutex;
    std::atomic<u32> alive_count{0};
    std::atomic<u32> aged_count{0};
    std::atomic<u32> culled_count{0};

    fuse::jobs::parallel_for(0u, soa.capacity, grain_size, [&](u32 index) {
        if (soa.alive_flags[index] == 0U) {
            return;
        }
        aged_count.fetch_add(1U, std::memory_order_relaxed);
        if (advance_age_and_cull_slot(soa, index, dt)) {
            std::lock_guard<std::mutex> guard(dead_mutex);
            result.dead_slots.push_back(index);
            culled_count.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        alive_count.fetch_add(1U, std::memory_order_relaxed);
    });

    soa.count = alive_count.load(std::memory_order_relaxed);
    result.aged = aged_count.load(std::memory_order_relaxed);
    result.alive_after = soa.count;
    result.culled = culled_count.load(std::memory_order_relaxed);
    recycle_slots(soa, result.dead_slots);
    return result;
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

SimStepResult simulate_step(ParticleSoA& soa, const ParticleEmitterDesc& desc, f32 dt, u32 grain_size) {
    SimStepResult result{};
    const SimStepPreflight preflight = preflight_simulate_step(soa, dt);
    if (preflight.skipped) {
        result.skipped = true;
        result.alive_after = soa.count;
        return result;
    }

    result.dead_slots.reserve(soa.capacity / 8u + 1u);
    std::mutex dead_mutex;
    std::atomic<u32> alive_count{0};
    std::atomic<u32> integrated_count{0};
    std::atomic<u32> culled_count{0};

    fuse::jobs::parallel_for(0u, soa.capacity, grain_size, [&](u32 index) {
        if (soa.alive_flags[index] == 0U) {
            return;
        }
        integrate_slot(soa, index, desc, dt, result.dead_slots, dead_mutex, alive_count, integrated_count,
                       culled_count);
    });

    soa.count = alive_count.load(std::memory_order_relaxed);
    result.integrated = integrated_count.load(std::memory_order_relaxed);
    result.culled = culled_count.load(std::memory_order_relaxed);
    result.alive_after = soa.count;
    recycle_slots(soa, result.dead_slots);
    return result;
}

} // namespace fuse::vfx::particle_soa
