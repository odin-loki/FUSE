#pragma once

#include <fuse/renderer/cuda/vk_sync.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

struct InteropFillDesc {
    void* exportedMemoryHandle = nullptr;
    u64 allocationSize = 0;
    u32 width = 0;
    u32 height = 0;
    void* cudaStream = nullptr;
    /// Solid RGBA fill when toolkit launches the kernel (default: CUDA-ish blue).
    u8 rgba[4] = {13, 38, 89, 255};
    FrameSyncPair* frameSync = nullptr;
    u64 frameIndex = 0;
};

struct InteropFillResult {
    bool ok = false;
    bool stubPath = false;
    const char* reason = nullptr;
};

/// True when a CUDA fill kernel can run (toolkit + interop runtime).
bool interopFillAvailable();

/// Synchronous fill on the calling thread's CUDA stream (render-thread scaffold).
InteropFillResult fillInteropTexture(const InteropFillDesc& desc);

/// Job-lane path: waits on `FrameSyncPair`, fills, signals completion; uses `submit_cuda`.
InteropFillResult submitInteropFillJob(const InteropFillDesc& desc);

} // namespace fuse::renderer::cuda
