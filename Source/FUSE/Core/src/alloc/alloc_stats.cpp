#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

namespace {

StatsHookFn g_statsHook = nullptr;
void* g_statsHookUserData = nullptr;

} // namespace

void setGlobalStatsHook(StatsHookFn hook, void* userData) {
    g_statsHook = hook;
    g_statsHookUserData = userData;
}

void clearGlobalStatsHook() {
    g_statsHook = nullptr;
    g_statsHookUserData = nullptr;
}

void notifyStats(const char* allocatorName, const AllocStats& stats) {
    if (g_statsHook != nullptr) {
        g_statsHook(allocatorName, stats, g_statsHookUserData);
    }
}

namespace detail {

void recordBumpAlloc(AllocStats& stats, usize usedBytes, usize totalCapacity) {
    stats.usedBytes = usedBytes;
    stats.totalBytes = totalCapacity;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordBlockAlloc(AllocStats& stats, usize blockBytes, usize totalCapacity) {
    stats.usedBytes += blockBytes;
    stats.totalBytes = totalCapacity;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordFree(AllocStats& stats, usize bytes) {
    if (bytes <= stats.usedBytes) {
        stats.usedBytes -= bytes;
    } else {
        stats.usedBytes = 0;
    }
    stats.freeCount += 1;
}

void recordFailedAlloc(AllocStats& stats) {
    stats.failedAllocs += 1;
}

void updatePeak(AllocStats& stats) {
    if (stats.usedBytes > stats.peakUsedBytes) {
        stats.peakUsedBytes = stats.usedBytes;
    }
}

} // namespace detail

} // namespace fuse::alloc
