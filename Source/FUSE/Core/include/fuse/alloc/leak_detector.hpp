#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::alloc {

/// The engine allocators (pool, free list, checkedMalloc) feed the leak detector automatically in
/// debug builds only; the explicit API below works in every configuration.
#if defined(FUSE_DEBUG) && FUSE_DEBUG
inline constexpr bool kLeakDetectorEnabled = true;
#else
inline constexpr bool kLeakDetectorEnabled = false;
#endif

struct LeakRecord {
    const void* ptr = nullptr;
    usize size = 0;
    const char* tag = nullptr;
    const char* file = nullptr;
    u32 line = 0;
};

struct LeakReport {
    u64 liveCount = 0;
    usize liveBytes = 0;
    /// Allocations that could not be tracked because the fixed table was full.
    u64 untracked = 0;
    /// recordFree() calls for pointers that were never recorded (double or foreign frees).
    u64 unknownFrees = 0;
};

/// Live-allocation registry (B7.8). Fixed-capacity table: recording never touches the heap, so the
/// zero-allocation hot paths stay zero-allocation with tracking on. Thread-safe.
class LeakDetector {
public:
    static constexpr u32 kCapacity = 1u << 16;

    static void recordAlloc(const void* ptr, usize size, const char* tag, const char* file = nullptr,
                            u32 line = 0);
    /// Returns false when `ptr` was not live (counted in LeakReport::unknownFrees).
    static bool recordFree(const void* ptr);
    /// An arena was reset or released: every allocation inside [begin, begin + bytes) is gone.
    static void releaseRange(const void* begin, usize bytes);

    static u64 liveCount();
    static usize liveBytes();
    static std::vector<LeakRecord> liveRecords();

    /// Logs every live allocation (Error level, first 32 in full; silent when clean) and returns the
    /// totals. Called by fuse::core::shutdown() in debug builds.
    static LeakReport reportLeaks();

    /// Totals without logging.
    static LeakReport snapshot();

    /// Forget everything (tests).
    static void clear();
};

} // namespace fuse::alloc
