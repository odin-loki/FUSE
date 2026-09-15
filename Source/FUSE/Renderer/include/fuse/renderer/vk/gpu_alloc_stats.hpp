#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

struct TextureDesc;

/// GPU allocator counters for debug overlays and budget checks (B2.3 deepen).
struct GpuAllocStats {
    usize usedBytes = 0;
    usize bufferBytes = 0;
    usize imageBytes = 0;
    usize peakUsedBytes = 0;
    u32 bufferCount = 0;
    u32 imageCount = 0;
    u64 allocCount = 0;
    u64 freeCount = 0;
    u64 failedAllocs = 0;
    /// Populated when VMA is active; stub path leaves at zero.
    u32 vmaPoolCount = 0;
    usize vmaPoolUsedBytes = 0;
};

using GpuStatsHookFn = void (*)(const char* allocatorName, const GpuAllocStats& stats, void* userData);

void setGlobalGpuStatsHook(GpuStatsHookFn hook, void* userData = nullptr);
void clearGlobalGpuStatsHook();
void notifyGpuStats(const char* allocatorName, const GpuAllocStats& stats);

namespace gpu_alloc_detail {

void recordBufferAlloc(GpuAllocStats& stats, usize bytes);
void recordImageAlloc(GpuAllocStats& stats, usize bytes);
void recordBufferFree(GpuAllocStats& stats, usize bytes);
void recordImageFree(GpuAllocStats& stats, usize bytes);
void recordFailedAlloc(GpuAllocStats& stats);
void updatePeak(GpuAllocStats& stats);

usize estimateImageBytes(const TextureDesc& desc);

} // namespace gpu_alloc_detail

} // namespace fuse::renderer
