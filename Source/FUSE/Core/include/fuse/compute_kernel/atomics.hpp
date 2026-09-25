#pragma once

// Single-source atomics for kernel bodies (device-safe header).
//
//   scratch_atomic_add  workgroup scratch. On the CPU a workgroup runs on one thread, so a plain
//                       add is exact; on CUDA it is atomicAdd on __shared__ memory.
//   global_atomic_add   grid-visible memory written by several workgroups (CpuParallel runs
//                       workgroups concurrently): std::atomic_ref on the host, atomicAdd on CUDA.
//
// Integer adds are order-independent, so results stay bit-exact across backends. Float atomics
// are not provided on purpose: their result depends on ordering — reduce in scratch instead.

#include <fuse/types.hpp>

#if !defined(__CUDA_ARCH__)
#include <atomic>
#endif

namespace fuse::kernel {

FUSE_HOST_DEVICE inline u32 scratch_atomic_add(u32* address, u32 value) {
#if defined(__CUDA_ARCH__)
    return atomicAdd(address, value);
#else
    const u32 old = *address;
    *address = old + value;
    return old;
#endif
}

FUSE_HOST_DEVICE inline u32 global_atomic_add(u32* address, u32 value) {
#if defined(__CUDA_ARCH__)
    return atomicAdd(address, value);
#else
    return std::atomic_ref<u32>(*address).fetch_add(value, std::memory_order_relaxed);
#endif
}

} // namespace fuse::kernel
