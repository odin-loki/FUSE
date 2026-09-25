#include <fuse/alloc/leak_detector.hpp>

#include <fuse/log/logger.hpp>

#include <cstdint>
#include <mutex>

namespace fuse::alloc {

namespace {

// Open addressing, linear probing, backward-shift deletion (no tombstones). Static storage so
// recording never allocates.
constexpr u32 kMask = LeakDetector::kCapacity - 1u;
constexpr u32 kMaxLoad = LeakDetector::kCapacity - LeakDetector::kCapacity / 4u;

struct Table {
    LeakRecord slots[LeakDetector::kCapacity];
    u32 count = 0;
    usize bytes = 0;
    u64 untracked = 0;
    u64 unknownFrees = 0;
};

Table g_table;
std::mutex g_mutex;

u32 home(const void* ptr) {
    const u64 key = reinterpret_cast<u64>(ptr) >> 4;
    return static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 48) & kMask;
}

bool findLocked(const void* ptr, u32& outSlot) {
    u32 slot = home(ptr);
    for (u32 probe = 0; probe < LeakDetector::kCapacity; ++probe) {
        const LeakRecord& record = g_table.slots[slot];
        if (record.ptr == nullptr) {
            return false;
        }
        if (record.ptr == ptr) {
            outSlot = slot;
            return true;
        }
        slot = (slot + 1u) & kMask;
    }
    return false;
}

// Removes slot `hole` and shifts later members of the probe run back so lookups stay correct.
void eraseLocked(u32 hole) {
    g_table.bytes -= g_table.slots[hole].size;
    --g_table.count;
    u32 next = (hole + 1u) & kMask;
    while (g_table.slots[next].ptr != nullptr) {
        const u32 want = home(g_table.slots[next].ptr);
        // Move `next` into the hole unless its home lies cyclically in (hole, next].
        const bool homeBetween = hole <= next ? (hole < want && want <= next) : (hole < want || want <= next);
        if (!homeBetween) {
            g_table.slots[hole] = g_table.slots[next];
            hole = next;
        }
        next = (next + 1u) & kMask;
    }
    g_table.slots[hole] = LeakRecord{};
}

LeakReport snapshotLocked() {
    LeakReport report;
    report.liveCount = g_table.count;
    report.liveBytes = g_table.bytes;
    report.untracked = g_table.untracked;
    report.unknownFrees = g_table.unknownFrees;
    return report;
}

} // namespace

void LeakDetector::recordAlloc(const void* ptr, usize size, const char* tag, const char* file, u32 line) {
    if (ptr == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_mutex);
    u32 slot = 0;
    if (findLocked(ptr, slot)) {
        // Address reused without a recorded free (e.g. an untracked release): replace the record.
        g_table.bytes -= g_table.slots[slot].size;
        g_table.bytes += size;
        g_table.slots[slot] = LeakRecord{ptr, size, tag, file, line};
        return;
    }
    if (g_table.count >= kMaxLoad) {
        ++g_table.untracked;
        return;
    }
    slot = home(ptr);
    while (g_table.slots[slot].ptr != nullptr) {
        slot = (slot + 1u) & kMask;
    }
    g_table.slots[slot] = LeakRecord{ptr, size, tag, file, line};
    ++g_table.count;
    g_table.bytes += size;
}

bool LeakDetector::recordFree(const void* ptr) {
    if (ptr == nullptr) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(g_mutex);
    u32 slot = 0;
    if (!findLocked(ptr, slot)) {
        ++g_table.unknownFrees;
        return false;
    }
    eraseLocked(slot);
    return true;
}

void LeakDetector::releaseRange(const void* begin, usize bytes) {
    if (begin == nullptr || bytes == 0u) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_table.count == 0u) {
        return;
    }
    const auto lo = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t hi = lo + bytes;
    // Backward-shift deletion only moves entries into the slot being examined or into slots not
    // yet visited, so re-examining slot i after an erase visits every entry.
    for (u32 i = 0; i < kCapacity;) {
        const auto p = reinterpret_cast<uintptr_t>(g_table.slots[i].ptr);
        if (p != 0u && p >= lo && p < hi) {
            eraseLocked(i);
            continue;
        }
        ++i;
    }
}

u64 LeakDetector::liveCount() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_table.count;
}

usize LeakDetector::liveBytes() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_table.bytes;
}

std::vector<LeakRecord> LeakDetector::liveRecords() {
    std::vector<LeakRecord> out;
    const std::lock_guard<std::mutex> lock(g_mutex);
    out.reserve(g_table.count);
    for (const LeakRecord& record : g_table.slots) {
        if (record.ptr != nullptr) {
            out.push_back(record);
        }
    }
    return out;
}

LeakReport LeakDetector::snapshot() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return snapshotLocked();
}

LeakReport LeakDetector::reportLeaks() {
    const std::vector<LeakRecord> records = liveRecords();
    const LeakReport report = snapshot();
    auto& logger = log::Logger::instance();
    constexpr usize kDetailed = 32u;
    for (usize i = 0; i < records.size() && i < kDetailed; ++i) {
        const LeakRecord& r = records[i];
        logger.log(log::Level::Error, "leak: %zu bytes at %p tag=%s %s:%u", r.size, r.ptr,
                   r.tag != nullptr ? r.tag : "-", r.file != nullptr ? r.file : "-", r.line);
    }
    if (report.liveCount != 0u || report.untracked != 0u || report.unknownFrees != 0u) {
        logger.log(log::Level::Error, "leak report: %llu live allocations, %zu bytes (%llu untracked, %llu unknown frees)",
                   static_cast<unsigned long long>(report.liveCount), report.liveBytes,
                   static_cast<unsigned long long>(report.untracked),
                   static_cast<unsigned long long>(report.unknownFrees));
    }
    return report;
}

void LeakDetector::clear() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (LeakRecord& record : g_table.slots) {
        record = LeakRecord{};
    }
    g_table.count = 0;
    g_table.bytes = 0;
    g_table.untracked = 0;
    g_table.unknownFrees = 0;
}

} // namespace fuse::alloc
