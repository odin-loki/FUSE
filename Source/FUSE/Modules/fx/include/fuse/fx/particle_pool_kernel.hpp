#pragma once

// Single-source integrate step of the AFX particle pool (docs/compute-kernels.md, "Particles"). The
// per-particle math lives once in `integrate_particle`; two item kernels apply it to the two layouts
// the pool has: the CPU `ParticleSlot` array (ParticlePool::tick) and the 40-byte packed SSBO mirror
// the CUDA backend keeps resident (ParticlePoolGpuBackend / src/particle_pool_cuda.cu). Both run on
// every kernel backend, so the packed GPU path is checked against the CPU pool with run_parity.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/fx/particle_pool.hpp>
#include <fuse/types.hpp>

namespace fuse::fx::particle_pool_kernel {

/// Kernel / profiler / GPU-timestamp names.
inline constexpr const char* kSlotName = "fx_particle_pool_integrate";
inline constexpr const char* kPackedName = "fx_particle_pool_integrate_packed";
inline constexpr kernel::Dim3 kWorkgroup{64u, 1u, 1u};

/// Packed slot: position(12) + velocity(12) + lifetime(4) + age(4) + blend(4) + alive(4).
inline constexpr u32 kBytesPerSlot = 40u;
inline constexpr u32 kFloatsPerSlot = 9u; ///< Floats before the alive flag.
inline constexpr u32 kAliveOffset = 36u;

/// The pool's step length: non-positive frame dt falls back to 1/60 s.
FUSE_HOST_DEVICE inline f32 resolve_dt(f32 dt) {
    return dt > 0.f ? dt : (1.f / 60.f);
}

/// Advances one live particle by `dt` (age, then position); true when it reached its lifetime.
FUSE_HOST_DEVICE inline bool integrate_particle(f32& px, f32& py, f32& pz, f32 vx, f32 vy, f32 vz, f32& age,
                                                f32 lifetime, f32 dt) {
    age += dt;
    px += vx * dt;
    py += vy * dt;
    pz += vz * dt;
    return age >= lifetime;
}

// ---- CPU pool layout (ParticleSlot) ----------------------------------------------------------------

struct SlotParams {
    kernel::Span<ParticleSlot> slots;
    u32* expired = nullptr; ///< Incremented once per particle that died this step.
    f32 dt = 0.f;           ///< Already resolved (resolve_dt).
};

struct SlotKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SlotParams& p) const {
        ParticleSlot& slot = p.slots[idx.linear];
        if (!slot.alive) {
            return;
        }
        if (integrate_particle(slot.position.x, slot.position.y, slot.position.z, slot.velocity.x, slot.velocity.y,
                               slot.velocity.z, slot.age, slot.lifetime, p.dt)) {
            slot.alive = false;
            kernel::global_atomic_add(p.expired, 1u);
        }
    }
};

inline kernel::KernelLaunch slot_launch(u32 slot_count) {
    return kernel::KernelLaunch{kSlotName, kernel::extent1(slot_count), kWorkgroup};
}

// ---- packed SSBO layout ------------------------------------------------------------------------------

/// Byte copy usable on host and device (well-defined type punning of the packed byte buffer).
FUSE_HOST_DEVICE inline void copy_bytes(void* dst, const void* src, u32 n) {
    u8* d = static_cast<u8*>(dst);
    const u8* s = static_cast<const u8*>(src);
    for (u32 i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

struct PackedParams {
    kernel::Span<u8> packed; ///< slot_count * kBytesPerSlot bytes.
    f32 dt = 0.f;            ///< Already resolved (resolve_dt).
};

struct PackedKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PackedParams& p) const {
        u8* slot = p.packed.data + static_cast<usize>(idx.linear) * kBytesPerSlot;
        u32 alive = 0u;
        copy_bytes(&alive, slot + kAliveOffset, static_cast<u32>(sizeof(u32)));
        if (alive == 0u) {
            return;
        }
        f32 f[kFloatsPerSlot];
        copy_bytes(f, slot, static_cast<u32>(sizeof(f)));
        // f: px py pz vx vy vz lifetime age blend
        if (integrate_particle(f[0], f[1], f[2], f[3], f[4], f[5], f[7], f[6], p.dt)) {
            alive = 0u;
            copy_bytes(slot + kAliveOffset, &alive, static_cast<u32>(sizeof(u32)));
        }
        copy_bytes(slot, f, static_cast<u32>(sizeof(f)));
    }
};

inline kernel::KernelLaunch packed_launch(u32 slot_count) {
    return kernel::KernelLaunch{kPackedName, kernel::extent1(slot_count), kWorkgroup};
}

} // namespace fuse::fx::particle_pool_kernel
