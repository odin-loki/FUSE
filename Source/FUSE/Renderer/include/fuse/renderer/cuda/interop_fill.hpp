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

/// Batch stub-path exercise for CI (WP-06i) — counts stub vs ok without requiring handles.
struct InteropFillLoadStressResult {
    u32 attempts = 0;
    u32 stubPaths = 0;
    u32 failures = 0;
    u32 successes = 0;
};

[[nodiscard]] InteropFillLoadStressResult stressInteropFillUnderLoad(u32 iterations);

/// Combined frame-sync + interop-fill stress (WP-06n) — bookkeeping always advances; driver ops when wired.
struct FrameSyncInteropCombinedStressResult {
    FrameSyncLoadStressResult frameSync{};
    InteropFillLoadStressResult interopFill{};
    u32 jobLaneFillAttempts = 0;
    u32 jobLaneFillStubPaths = 0;
};

[[nodiscard]] FrameSyncInteropCombinedStressResult stressFrameSyncAndInteropFillUnderLoad(
    u32 frameCount);

} // namespace fuse::renderer::cuda
