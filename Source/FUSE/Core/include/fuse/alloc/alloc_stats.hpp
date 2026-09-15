#pragma once

#include <fuse/types.hpp>

namespace fuse::alloc {

/// Per-allocator counters surfaced for debug overlays and budget checks (B1.3).
struct AllocStats {
    usize usedBytes = 0;
    usize totalBytes = 0;
    usize peakUsedBytes = 0;
    u64 allocCount = 0;
    u64 freeCount = 0;
    u64 failedAllocs = 0;
};

using StatsHookFn = void (*)(const char* allocatorName, const AllocStats& stats, void* userData);

/// Optional global hook invoked when allocators report stats (frame advance, reset, etc.).
void setGlobalStatsHook(StatsHookFn hook, void* userData = nullptr);
void clearGlobalStatsHook();

/// Notify the global hook, if registered. No-op when hook is unset.
void notifyStats(const char* allocatorName, const AllocStats& stats);

namespace detail {

void recordBumpAlloc(AllocStats& stats, usize usedBytes, usize totalCapacity);
void recordBlockAlloc(AllocStats& stats, usize blockBytes, usize totalCapacity);
void recordFree(AllocStats& stats, usize bytes);
void recordFailedAlloc(AllocStats& stats);
void updatePeak(AllocStats& stats);

} // namespace detail

} // namespace fuse::alloc
