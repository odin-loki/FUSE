// B1.8 gate (FUSE_MASTER_PLAN "All allocators pass 1M alloc/free stress cycles — zero leaks,
// alignment always correct"):
//
// Every engine allocator type (Pool, FreeList, Ring, Stack, Frame) runs exactly 1,000,000 FULL
// alloc -> free cycles (1M successful allocations, each one later released: 2M operations), driven
// through the IAllocator interface with random sizes/alignments and a randomly breathing working
// set. Throughout the run the test checks:
//   - alignment of every returned block (1..256),
//   - no overlap/corruption (each live block is stamped and verified on release),
//   - allocator invariants every 1024 operations (allocCount - freeCount == live blocks,
//     usedBytes consistent with the live set, peak <= capacity, allocator-specific checks),
//   - LeakDetector live-record delta == live tracked blocks (Debug builds, where Pool and
//     FreeList feed the detector), and zero live records / zero unknown frees at the end,
//   - final stats: allocCount == freeCount == 1M, usedBytes == 0, zero failed allocations where
//     failure is not part of the allocator's contract.

#include <fuse/alloc/allocator.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/alloc/freelist_allocator.hpp>
#include <fuse/alloc/leak_detector.hpp>
#include <fuse/alloc/pool_allocator.hpp>
#include <fuse/alloc/ring_allocator.hpp>
#include <fuse/alloc/stack_allocator.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace {

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::alloc::AllocInfo;
using fuse::alloc::IAllocator;
using fuse::alloc::LeakDetector;

constexpr u64 kCycles = 1'000'000u;
constexpr u32 kCheckEvery = 1024u;

int g_failures = 0;

void expectTrue(bool condition, const char* allocator, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL [%s]: %s\n", allocator, message);
        ++g_failures;
    }
}

struct LiveBlock {
    void* ptr = nullptr;
    u32 size = 0;
    u32 serial = 0;
    u32 mark = 0; // stack only: marker taken before the allocation
};

u8 fillByte(u32 serial) {
    return static_cast<u8>((serial * 131u) ^ 0x5Au);
}

void stamp(const LiveBlock& b) {
    std::memset(b.ptr, fillByte(b.serial), b.size);
}

bool verify(const LiveBlock& b) {
    const auto* p = static_cast<const u8*>(b.ptr);
    const u8 expected = fillByte(b.serial);
    for (u32 i = 0; i < b.size; ++i) {
        if (p[i] != expected) {
            return false;
        }
    }
    return true;
}

bool isAligned(const void* ptr, usize alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1u)) == 0u;
}

u32 randomAlignment(std::mt19937& rng) {
    return 1u << (rng() % 9u); // 1 .. 256
}

struct Tally {
    u64 allocs = 0;
    u64 frees = 0;
    u64 misaligned = 0;
    u64 corrupted = 0;
    u64 invariantViolations = 0;
    u64 leakDetectorMismatches = 0;
    u64 checks = 0;
};

void report(const char* name, const Tally& t, const fuse::alloc::AllocStats& s) {
    std::printf("  %-8s cycles=%llu (allocs=%llu frees=%llu) stats.alloc=%llu stats.free=%llu used=%llu peak=%llu/%llu "
                "failed=%llu misaligned=%llu corrupted=%llu invariant-checks=%llu violations=%llu leakdet-mismatch=%llu\n",
                name, static_cast<unsigned long long>(t.frees), static_cast<unsigned long long>(t.allocs),
                static_cast<unsigned long long>(t.frees), static_cast<unsigned long long>(s.allocCount),
                static_cast<unsigned long long>(s.freeCount), static_cast<unsigned long long>(s.usedBytes),
                static_cast<unsigned long long>(s.peakUsedBytes), static_cast<unsigned long long>(s.totalBytes),
                static_cast<unsigned long long>(s.failedAllocs), static_cast<unsigned long long>(t.misaligned),
                static_cast<unsigned long long>(t.corrupted), static_cast<unsigned long long>(t.checks),
                static_cast<unsigned long long>(t.invariantViolations),
                static_cast<unsigned long long>(t.leakDetectorMismatches));
}

/// Pool / FreeList feed the leak detector in Debug; the live record delta must match exactly.
void checkLeakDetector(Tally& t, u64 baseline, usize liveTracked) {
    if constexpr (fuse::alloc::kLeakDetectorEnabled) {
        if (LeakDetector::liveCount() != baseline + liveTracked) {
            ++t.leakDetectorMismatches;
        }
    }
}

void finalChecks(const char* name, const Tally& t, const fuse::alloc::AllocStats& s, bool expectFreeCount) {
    report(name, t, s);
    expectTrue(t.allocs == kCycles, name, "exactly 1M successful allocations");
    expectTrue(t.frees == kCycles, name, "exactly 1M releases (1M full alloc/free cycles)");
    expectTrue(s.allocCount == kCycles, name, "stats.allocCount == 1M");
    if (expectFreeCount) {
        expectTrue(s.freeCount == s.allocCount, name, "stats.freeCount == stats.allocCount");
    }
    expectTrue(s.usedBytes == 0u, name, "stats.usedBytes == 0 after every cycle released");
    expectTrue(s.peakUsedBytes <= s.totalBytes, name, "peak usage never exceeds capacity");
    expectTrue(t.misaligned == 0u, name, "every block honours the requested alignment");
    expectTrue(t.corrupted == 0u, name, "no overlapping/corrupted blocks");
    expectTrue(t.invariantViolations == 0u, name, "allocator invariants hold throughout the run");
    expectTrue(t.leakDetectorMismatches == 0u, name, "LeakDetector live records track live blocks exactly");
}

/// Decide the next step: allocate while below the (breathing) working-set target and 1M allocations
/// have not been reached; otherwise release one live block.
bool wantAlloc(std::mt19937& rng, const Tally& t, usize live, usize maxLive) {
    if (t.allocs >= kCycles) {
        return false;
    }
    if (live == 0u) {
        return true;
    }
    if (live >= maxLive) {
        return false;
    }
    return (rng() & 1u) != 0u;
}

// ---- Pool -------------------------------------------------------------------------------------

void runPool() {
    const char* name = "pool";
    constexpr u32 kBlock = 256u;
    constexpr u32 kCount = 512u;
    const u64 baseline = LeakDetector::liveCount();
    std::mt19937 rng(101u);
    fuse::alloc::PoolAllocator pool(kBlock, kCount, "cycles.pool");
    IAllocator& a = pool;
    std::vector<LiveBlock> live;
    live.reserve(kCount);
    Tally t;
    u32 serial = 0;
    u64 op = 0;
    while (t.frees < kCycles) {
        if (wantAlloc(rng, t, live.size(), kCount)) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % kBlock;
            void* p = a.alloc({size, align, nullptr});
            if (p == nullptr) {
                ++t.invariantViolations; // pool never fails below capacity
                continue;
            }
            ++t.allocs;
            t.misaligned += isAligned(p, align) ? 0u : 1u;
            t.invariantViolations += pool.isLive(p) ? 0u : 1u;
            LiveBlock b{p, size, ++serial, 0u};
            stamp(b);
            live.push_back(b);
        } else {
            const u32 idx = rng() % static_cast<u32>(live.size());
            t.corrupted += verify(live[idx]) ? 0u : 1u;
            void* p = live[idx].ptr;
            a.free(p, kBlock);
            t.invariantViolations += pool.isLive(p) ? 1u : 0u;
            ++t.frees;
            live[idx] = live.back();
            live.pop_back();
        }
        if ((++op % kCheckEvery) == 0u) {
            ++t.checks;
            const auto s = a.stats();
            const bool ok = s.allocCount - s.freeCount == live.size() && s.usedBytes == live.size() * kBlock &&
                            pool.availableBlocks() == kCount - live.size() && s.peakUsedBytes <= s.totalBytes &&
                            pool.doubleFreeCount() == 0u;
            t.invariantViolations += ok ? 0u : 1u;
            checkLeakDetector(t, baseline, live.size());
        }
    }
    const auto s = a.stats();
    expectTrue(pool.availableBlocks() == kCount, name, "every block returned to the pool");
    expectTrue(pool.doubleFreeCount() == 0u, name, "no double frees detected");
    expectTrue(s.failedAllocs == 0u, name, "no failed allocations");
    checkLeakDetector(t, baseline, 0u);
    finalChecks(name, t, s, true);
}

// ---- FreeList ---------------------------------------------------------------------------------

void runFreeList() {
    const char* name = "freelist";
    constexpr u32 kCapacity = 1u << 20;
    constexpr usize kMaxLive = 256u;
    const u64 baseline = LeakDetector::liveCount();
    std::mt19937 rng(202u);
    fuse::alloc::FreeListAllocator heap(kCapacity, "cycles.freelist");
    IAllocator& a = heap;
    std::vector<LiveBlock> live;
    live.reserve(kMaxLive);
    Tally t;
    u32 serial = 0;
    u64 op = 0;
    usize liveBytes = 0;
    while (t.frees < kCycles) {
        if (wantAlloc(rng, t, live.size(), kMaxLive)) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 2048u;
            void* p = a.alloc({size, align, nullptr});
            if (p == nullptr) {
                ++t.invariantViolations; // <= 256 * (2 KiB + overhead) always fits in 1 MiB
                continue;
            }
            ++t.allocs;
            t.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial, 0u};
            stamp(b);
            live.push_back(b);
            liveBytes += size;
        } else {
            const u32 idx = rng() % static_cast<u32>(live.size());
            t.corrupted += verify(live[idx]) ? 0u : 1u;
            a.free(live[idx].ptr, live[idx].size);
            liveBytes -= live[idx].size;
            ++t.frees;
            live[idx] = live.back();
            live.pop_back();
        }
        if ((++op % kCheckEvery) == 0u) {
            ++t.checks;
            const auto s = a.stats();
            // usedBytes includes headers/padding, so it bounds the payload from above.
            const bool ok = s.allocCount - s.freeCount == live.size() && s.usedBytes >= liveBytes &&
                            s.usedBytes <= s.totalBytes && (live.empty() == (s.usedBytes == 0u)) &&
                            s.peakUsedBytes <= s.totalBytes;
            t.invariantViolations += ok ? 0u : 1u;
            checkLeakDetector(t, baseline, live.size());
        }
    }
    const auto s = a.stats();
    // Fully coalesced: a block spanning nearly the whole arena must fit again.
    void* whole = heap.allocate(kCapacity - 64u, 16u);
    expectTrue(whole != nullptr, name, "arena fully coalesces after 1M cycles");
    if (whole != nullptr) {
        a.free(whole, kCapacity - 64u);
    }
    expectTrue(heap.usedBytes() == 0u, name, "arena empty after coalescing probe");
    expectTrue(s.failedAllocs == 0u, name, "no failed allocations");
    checkLeakDetector(t, baseline, 0u);
    finalChecks(name, t, s, true);
}

// ---- Ring -------------------------------------------------------------------------------------

void runRing() {
    const char* name = "ring";
    constexpr u32 kCapacity = 64u * 1024u;
    std::mt19937 rng(303u);
    fuse::alloc::RingAllocator ring(kCapacity, "cycles.ring");
    IAllocator& a = ring;
    std::deque<LiveBlock> live;
    Tally t;
    u32 serial = 0;
    u64 op = 0;
    u64 expectedFailures = 0;
    usize liveBytes = 0;
    auto releaseFront = [&] {
        t.corrupted += verify(live.front()) ? 0u : 1u;
        a.free(live.front().ptr, live.front().size);
        liveBytes -= live.front().size;
        ++t.frees;
        live.pop_front();
    };
    while (t.frees < kCycles) {
        if (wantAlloc(rng, t, live.size(), 1024u)) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 1024u;
            void* p = a.alloc({size, align, nullptr});
            if (p == nullptr) {
                // Ring full is part of the contract: retire the oldest block (FIFO) and retry later.
                ++expectedFailures;
                if (live.empty()) {
                    ++t.invariantViolations; // an empty 64 KiB ring must fit a <= 1 KiB block
                } else {
                    releaseFront();
                }
            } else {
                ++t.allocs;
                t.misaligned += isAligned(p, align) ? 0u : 1u;
                LiveBlock b{p, size, ++serial, 0u};
                stamp(b);
                live.push_back(b);
                liveBytes += size;
            }
        } else {
            releaseFront();
        }
        if ((++op % kCheckEvery) == 0u) {
            ++t.checks;
            const auto s = a.stats();
            const bool ok = s.allocCount - s.freeCount == live.size() && s.usedBytes >= liveBytes &&
                            s.usedBytes <= s.totalBytes && (live.empty() == (s.usedBytes == 0u)) &&
                            s.failedAllocs == expectedFailures && ring.failedAllocations() == s.failedAllocs;
            t.invariantViolations += ok ? 0u : 1u;
        }
    }
    const auto s = a.stats();
    expectTrue(s.failedAllocs == expectedFailures, name, "failedAllocs counts exactly the ring-full events");
    finalChecks(name, t, s, true);
}

// ---- Stack ------------------------------------------------------------------------------------

void runStack() {
    const char* name = "stack";
    constexpr u32 kCapacity = 256u * 1024u;
    constexpr usize kMaxLive = 256u; // 256 * (512 + 255 padding) < 256 KiB
    std::mt19937 rng(404u);
    fuse::alloc::StackAllocator stack(kCapacity, "cycles.stack");
    IAllocator& a = stack;
    std::vector<LiveBlock> live;
    live.reserve(kMaxLive);
    Tally t;
    u32 serial = 0;
    u64 op = 0;
    while (t.frees < kCycles) {
        if (wantAlloc(rng, t, live.size(), kMaxLive)) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 512u;
            const fuse::alloc::StackMarker mark = stack.pushMark();
            void* p = a.alloc({size, align, nullptr});
            if (p == nullptr) {
                ++t.invariantViolations;
                continue;
            }
            ++t.allocs;
            t.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial, mark};
            stamp(b);
            live.push_back(b);
        } else {
            const LiveBlock& b = live.back();
            t.corrupted += verify(b) ? 0u : 1u;
            // LIFO release; odd serials use free(ptr,size), even ones roll back to the marker
            // (which also returns the alignment padding).
            if ((b.serial & 1u) != 0u) {
                a.free(b.ptr, b.size);
                if (stack.pushMark() != b.mark) {
                    // free() returns the payload only; roll back the alignment padding too (this
                    // records one extra release in stats.freeCount).
                    stack.popToMark(b.mark);
                }
            } else {
                stack.popToMark(b.mark);
            }
            t.invariantViolations += stack.pushMark() == b.mark ? 0u : 1u;
            ++t.frees;
            live.pop_back();
        }
        if ((++op % kCheckEvery) == 0u) {
            ++t.checks;
            const auto s = a.stats();
            const bool ok = s.usedBytes == stack.pushMark() && s.usedBytes <= s.totalBytes &&
                            (live.empty() == (s.usedBytes == 0u)) && s.peakUsedBytes <= s.totalBytes &&
                            s.failedAllocs == 0u;
            t.invariantViolations += ok ? 0u : 1u;
        }
    }
    const auto s = a.stats();
    expectTrue(stack.pushMark() == 0u, name, "stack fully unwound (top == 0)");
    expectTrue(s.failedAllocs == 0u, name, "no failed allocations or bad pops");
    // Padding roll-backs after free() are extra recorded releases, so freeCount >= allocCount.
    expectTrue(s.freeCount >= s.allocCount, name, "every allocation has a recorded release");
    finalChecks(name, t, s, false);
}

// ---- Frame ------------------------------------------------------------------------------------

void runFrame() {
    const char* name = "frame";
    constexpr u32 kCapacity = 64u * 1024u;
    std::mt19937 rng(505u);
    fuse::alloc::FrameAllocator frame(kCapacity, "cycles.frame");
    IAllocator& a = frame;
    std::vector<LiveBlock> current;
    std::vector<LiveBlock> previous;
    current.reserve(4096u);
    previous.reserve(4096u);
    Tally t;
    u32 serial = 0;
    u64 frames = 0;
    usize currentBytes = 0;
    auto endFrame = [&] {
        // Frame N-1 must be intact right up to its recycling; then all of it is released at once.
        for (const LiveBlock& b : previous) {
            t.corrupted += verify(b) ? 0u : 1u;
        }
        for (const LiveBlock& b : current) {
            t.invariantViolations += frame.isLive(b.ptr) ? 0u : 1u;
        }
        t.frees += previous.size();
        previous.swap(current);
        current.clear();
        frame.advanceFrame();
        ++frames;
        currentBytes = 0;
        ++t.checks;
        const auto s = a.stats();
        t.invariantViolations += (s.usedBytes == 0u && s.allocCount == t.allocs) ? 0u : 1u;
    };
    while (t.allocs < kCycles) {
        const u32 align = randomAlignment(rng);
        const u32 size = 1u + rng() % 512u;
        void* p = a.alloc({size, align, nullptr});
        if (p == nullptr) {
            endFrame(); // frame full: expected, counted in failedAllocs
            continue;
        }
        ++t.allocs;
        t.misaligned += isAligned(p, align) ? 0u : 1u;
        LiveBlock b{p, size, ++serial, 0u};
        stamp(b);
        current.push_back(b);
        currentBytes += size;
        const auto s = a.stats();
        t.invariantViolations += (s.usedBytes >= currentBytes && s.usedBytes <= s.totalBytes) ? 0u : 1u;
        if ((rng() % 64u) == 0u) {
            endFrame();
        }
    }
    endFrame();
    endFrame();
    const auto s = a.stats();
    expectTrue(frame.usedBytes() == 0u && frame.previousFrameUsedBytes() == 0u, name,
               "zero bytes retained after two empty frames");
    std::printf("  frame    %llu frames advanced\n", static_cast<unsigned long long>(frames));
    // FrameAllocator frees are bulk (advanceFrame); per-block freeCount is not part of its contract.
    finalChecks(name, t, s, false);
}

} // namespace

int main() {
    LeakDetector::clear();
    const auto before = LeakDetector::snapshot();

    runPool();
    runFreeList();
    runRing();
    runStack();
    runFrame();

    const auto after = LeakDetector::snapshot();
    std::printf("  LeakDetector (enabled=%s): live=%llu bytes=%llu untracked=%llu unknownFrees=%llu\n",
                fuse::alloc::kLeakDetectorEnabled ? "yes" : "no", static_cast<unsigned long long>(after.liveCount),
                static_cast<unsigned long long>(after.liveBytes), static_cast<unsigned long long>(after.untracked),
                static_cast<unsigned long long>(after.unknownFrees));
    expectTrue(after.liveCount == before.liveCount && after.liveBytes == before.liveBytes, "leakdetector",
               "no live allocations remain");
    expectTrue(after.unknownFrees == before.unknownFrees, "leakdetector", "no unknown/double frees");
    expectTrue(after.untracked == before.untracked, "leakdetector", "every allocation was tracked");

    if (g_failures == 0) {
        std::printf("fuse_core_b1_alloc_million_cycles: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_alloc_million_cycles: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
