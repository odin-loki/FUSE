#pragma once

// Single-source particle simulation (docs/compute-kernels.md, "Particles": item update + workgroup
// compaction). These bodies are the ONLY implementation of the per-particle step: the CPU backends
// (src/particle_soa_ops.cpp) and the CUDA trampolines (kernels/particle_sim.cu) all run them.
//
// One simulation step is two launches over the SoA capacity with the same 256-wide workgroups:
//
//   1. "particle_update" (item kernel, one slot per item). A live slot ages by dt / lifetime; an
//      expired slot is marked kDiedFlag and counted in its workgroup's `group_dead[group]` and in the
//      global culled counter (global_atomic_add). A survivor optionally integrates (gravity, drag,
//      analytic colliders, attribute interpolation) and bumps the alive counter.
//   2. "particle_compact" (workgroup kernel) — count -> scan -> write, so the dead-slot list has the
//      same order on every backend however workgroups were scheduled:
//        phase 0  per-thread died flag -> scratch; partial sums of the preceding workgroups' counts
//        phase 1  thread 0: exclusive scan of the flags + the workgroup's base offset
//        phase 2  died slots write their index to dead_slots[total - 1 - rank] (descending, so the
//                 free list — popped from the back — hands out the lowest index first) and clear
//                 the died mark.
//
// Integer atomics are order-independent and each slot's float math is per-item, so CpuReference,
// CpuParallel (any worker count) produce bit-identical SoA columns, counters and dead lists.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/particle_emitter.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::vfx::particle_sim_kernel {

/// Kernel / profiler / GPU-timestamp names.
inline constexpr const char* kUpdateName = "particle_update";
inline constexpr const char* kCompactName = "particle_compact";

inline constexpr u32 kGroupSize = 256u;
inline constexpr kernel::Dim3 kWorkgroup{kGroupSize, 1u, 1u};

/// alive_flags values: 0 dead, 1 alive, 2 died during the current update (cleared by compaction).
inline constexpr u32 kAliveFlag = 1u;
inline constexpr u32 kDiedFlag = 2u;

/// Global counters (u32, zeroed by the host before the update launch).
enum Counter : u32 {
    kCounterAlive = 0,  ///< Survivors of the update.
    kCounterCulled = 1, ///< Slots that expired (== dead_slots entries written).
    kCounterCount = 2,
};

/// Per-launch constants resolved on the host from ParticleEmitterDesc.
struct Sim {
    f32 dt = 0.f;
    u32 integrate = 1u; ///< 0: age + cull only (lifetime_cull).
    math::Vec3 gravity{};
    f32 drag = 0.f;
    f32 size_start = 0.f;
    f32 size_end = 0.f;
    math::Vec3 color_start{};
    math::Vec3 color_end{};
    f32 alpha_start = 0.f;
    f32 alpha_end = 0.f;
    f32 restitution = 0.f;
    f32 friction = 0.f;
};

struct Params {
    kernel::Span<math::Vec3> positions;
    kernel::Span<math::Vec3> velocities;
    kernel::Span<f32> ages;
    kernel::Span<const f32> lifetimes;
    kernel::Span<f32> sizes;
    kernel::Span<math::Vec3> colors;
    kernel::Span<f32> alphas;
    kernel::Span<u32> alive_flags;
    kernel::Span<const ParticleCollider> colliders; ///< Empty: no collision response.
    kernel::Span<u32> counters;                     ///< kCounterCount entries.
    kernel::Span<u32> group_dead;                   ///< One entry per workgroup (zeroed by the host).
    kernel::Span<u32> dead_slots;                   ///< >= counters[kCounterCulled] entries (compaction).
    Sim sim{};
};

// ---- per-particle math (the single implementation) ---------------------------------------------

/// Ages slot `i` by dt normalized by its lifetime; true when it expired.
FUSE_HOST_DEVICE inline bool advance_age(const Params& p, u32 i) {
    p.ages[i] += p.sim.dt / std::max(p.lifetimes[i], 1e-4f);
    return p.ages[i] >= 1.f;
}

FUSE_HOST_DEVICE inline f32 collider_distance(const ParticleCollider& collider, const math::Vec3& position,
                                              math::Vec3& gradient) {
    if (collider.shape == ParticleColliderShape::Sphere) {
        const math::Vec3 offset = position - collider.center;
        const f32 length = offset.length();
        gradient = length > 1e-6f ? offset * (1.f / length) : math::Vec3{0.f, 1.f, 0.f};
        return length - collider.radius;
    }
    gradient = collider.normal;
    return collider.normal.dot(position) - collider.offset;
}

/// Pushes the particle back onto every penetrated collider's surface and reflects the approaching
/// normal velocity (restitution) while damping the tangential part (friction), in collider order.
FUSE_HOST_DEVICE inline void resolve_collisions(const Params& p, math::Vec3& position, math::Vec3& velocity) {
    for (u32 c = 0; c < p.colliders.size; ++c) {
        math::Vec3 normal{};
        const f32 distance = collider_distance(p.colliders[c], position, normal);
        if (distance >= 0.f) {
            continue;
        }
        position = position - normal * distance;
        const f32 normal_speed = velocity.dot(normal);
        if (normal_speed < 0.f) {
            const math::Vec3 normal_velocity = normal * normal_speed;
            const math::Vec3 tangent_velocity = velocity - normal_velocity;
            velocity = tangent_velocity * (1.f - p.sim.friction) - normal_velocity * p.sim.restitution;
        }
    }
}

/// Velocity / position integration and attribute interpolation of a surviving slot.
FUSE_HOST_DEVICE inline void integrate(const Params& p, u32 i) {
    math::Vec3 velocity = p.velocities[i];
    velocity = velocity + p.sim.gravity * p.sim.dt;
    velocity = velocity * (1.f - p.sim.drag * p.sim.dt);
    math::Vec3 position = p.positions[i] + velocity * p.sim.dt;
    resolve_collisions(p, position, velocity);
    p.velocities[i] = velocity;
    p.positions[i] = position;

    const f32 t = p.ages[i];
    p.sizes[i] = p.sim.size_start + (p.sim.size_end - p.sim.size_start) * t;
    p.colors[i] = p.sim.color_start + (p.sim.color_end - p.sim.color_start) * t;
    p.alphas[i] = p.sim.alpha_start + (p.sim.alpha_end - p.sim.alpha_start) * t;
}

// ---- kernels -------------------------------------------------------------------------------------

struct UpdateKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 i = idx.linear;
        if (p.alive_flags[i] == 0u) {
            return;
        }
        if (advance_age(p, i)) {
            p.alive_flags[i] = kDiedFlag;
            kernel::global_atomic_add(&p.group_dead[idx.group.x], 1u);
            kernel::global_atomic_add(&p.counters[kCounterCulled], 1u);
            return;
        }
        if (p.sim.integrate != 0u) {
            integrate(p, i);
        }
        kernel::global_atomic_add(&p.counters[kCounterAlive], 1u);
    }
};

struct CompactKernel {
    using Scratch = u32;
    /// [0, 256) per-thread died flag -> exclusive rank; [256, 512) partial base sums; [512] base.
    static constexpr u32 kScratchCount = 2u * kGroupSize + 1u;
    static constexpr u32 kPhases = 3u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const Params& p) const {
        u32* rank = wg.scratch;
        u32* partial = wg.scratch + kGroupSize;
        u32& base = wg.scratch[2u * kGroupSize];
        const u32 local = idx.local_linear;
        const u32 group = idx.group.x;
        switch (wg.phase) {
        case 0: {
            rank[local] = (idx.active && p.alive_flags[idx.linear] == kDiedFlag) ? 1u : 0u;
            // Count: this thread's share of the dead counts of every preceding workgroup.
            u32 sum = 0u;
            for (u32 g = local; g < group; g += kGroupSize) {
                sum += p.group_dead[g];
            }
            partial[local] = sum;
            break;
        }
        case 1:
            // Scan: serial and in thread order, identical on every backend.
            if (local == 0u) {
                u32 running = 0u;
                u32 offset = 0u;
                for (u32 t = 0; t < kGroupSize; ++t) {
                    const u32 died = rank[t];
                    rank[t] = running;
                    running += died;
                    offset += partial[t];
                }
                base = offset;
            }
            break;
        default:
            // Write: descending slot order across the whole grid.
            if (idx.active && p.alive_flags[idx.linear] == kDiedFlag) {
                const u32 total = p.counters[kCounterCulled];
                const u32 position = base + rank[local];
                if (position < total && total <= p.dead_slots.size) {
                    p.dead_slots[total - 1u - position] = idx.linear;
                }
                p.alive_flags[idx.linear] = 0u;
            }
            break;
        }
    }
};

// ---- host helpers --------------------------------------------------------------------------------

inline u32 group_count(u32 capacity) {
    return kernel::div_up(capacity, kGroupSize);
}

inline kernel::KernelLaunch update_launch(u32 capacity) {
    return kernel::KernelLaunch{kUpdateName, kernel::extent1(capacity), kWorkgroup};
}

inline kernel::KernelLaunch compact_launch(u32 capacity) {
    return kernel::KernelLaunch{kCompactName, kernel::extent1(capacity), kWorkgroup};
}

/// Per-launch constants from the emitter description (`desc` may be null for age-only culling).
inline Sim make_sim(const ParticleEmitterDesc* desc, f32 dt, bool integrate) {
    Sim sim{};
    sim.dt = dt;
    sim.integrate = (integrate && desc != nullptr) ? 1u : 0u;
    if (desc != nullptr) {
        sim.gravity = desc->gravity;
        sim.drag = desc->drag;
        sim.size_start = desc->size_start;
        sim.size_end = desc->size_end;
        sim.color_start = desc->color_start;
        sim.color_end = desc->color_end;
        sim.alpha_start = desc->alpha_start;
        sim.alpha_end = desc->alpha_end;
        sim.restitution = desc->restitution;
        sim.friction = desc->friction;
    }
    return sim;
}

} // namespace fuse::vfx::particle_sim_kernel
