// B1.3 / B1.8 memory gates (FUSE_MASTER_PLAN "B1.8 — Phase 1 Deliverables & Test Suite"):
//   - All allocators pass 1M alloc/free stress cycles: zero leaks, alignment always correct
//   - FrameAllocator (LinearAllocator) ping-pongs between frames; no use-after-reset in debug
//   - PoolAllocator detects double-free via generation counter; asserts in debug
//   - Budget system asserts when a domain exceeds its declared limit in debug builds
//   - Hot path (job submit -> execute -> complete -> alloc from pool -> free) makes zero heap
//     allocations per iteration (counted through this binary's global operator new)

#include <fuse/alloc/domain_budget.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/alloc/freelist_allocator.hpp>
#include <fuse/alloc/pool_allocator.hpp>
#include <fuse/alloc/ring_allocator.hpp>
#include <fuse/alloc/stack_allocator.hpp>
#include <fuse/assert.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <random>
#include <vector>

// ---- global heap counter (whole binary) -------------------------------------------------------

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};
} // namespace

void* operator new(std::size_t size) {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1u) / align * align;
    if (void* p = std::aligned_alloc(align, rounded)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }

namespace {

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool isAligned(const void* ptr, usize alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1u)) == 0u;
}

u8 fillByte(u32 serial) {
    return static_cast<u8>((serial * 131u) ^ 0x5Au);
}

/// Fill a live block with a serial-derived byte and verify it on free: overlapping or corrupted
/// blocks fail the check.
struct LiveBlock {
    void* ptr = nullptr;
    u32 size = 0;
    u32 serial = 0;
};

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

constexpr u32 kStressOps = 1'000'000u;

u32 randomAlignment(std::mt19937& rng) {
    return 1u << (rng() % 9u); // 1 .. 256
}

struct StressResult {
    u64 allocs = 0;
    u64 frees = 0;
    u64 misaligned = 0;
    u64 corrupted = 0;
};

void report(const char* name, const StressResult& r, bool leakFree) {
    std::printf("  %-9s %7llu allocs %7llu frees  misaligned=%llu corrupted=%llu leak-free=%s\n", name,
                static_cast<unsigned long long>(r.allocs), static_cast<unsigned long long>(r.frees),
                static_cast<unsigned long long>(r.misaligned), static_cast<unsigned long long>(r.corrupted),
                leakFree ? "yes" : "NO");
}

void testPoolStress() {
    std::mt19937 rng(1u);
    constexpr u32 kBlock = 256u;
    constexpr u32 kCount = 512u;
    fuse::alloc::PoolAllocator pool(kBlock, kCount, "stress.pool");
    std::vector<LiveBlock> live;
    StressResult r;
    u32 serial = 0;
    for (u32 op = 0; op < kStressOps; ++op) {
        const bool doAlloc = live.empty() || (live.size() < kCount && (rng() & 1u) != 0u);
        if (doAlloc) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % kBlock;
            void* p = pool.alloc({size, align, nullptr});
            if (p == nullptr) {
                continue;
            }
            ++r.allocs;
            r.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial};
            stamp(b);
            live.push_back(b);
        } else {
            const u32 idx = rng() % static_cast<u32>(live.size());
            r.corrupted += verify(live[idx]) ? 0u : 1u;
            pool.free(live[idx].ptr, kBlock);
            ++r.frees;
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (const LiveBlock& b : live) {
        r.corrupted += verify(b) ? 0u : 1u;
        pool.free(b.ptr, kBlock);
        ++r.frees;
    }
    const bool leakFree = pool.availableBlocks() == kCount && pool.stats().usedBytes == 0u &&
                          pool.stats().allocCount == pool.stats().freeCount && pool.doubleFreeCount() == 0u;
    report("pool", r, leakFree);
    expectTrue(r.allocs + r.frees >= kStressOps, "pool: 1M alloc/free operations");
    expectTrue(r.misaligned == 0u, "pool: every block honours the requested alignment");
    expectTrue(r.corrupted == 0u, "pool: no overlapping/corrupted blocks");
    expectTrue(leakFree, "pool: zero leaks after stress");
}

void testFreeListStress() {
    std::mt19937 rng(2u);
    constexpr u32 kCapacity = 1u << 20;
    fuse::alloc::FreeListAllocator heap(kCapacity, "stress.freelist");
    std::vector<LiveBlock> live;
    StressResult r;
    u32 serial = 0;
    for (u32 op = 0; op < kStressOps; ++op) {
        const bool doAlloc = live.empty() || (live.size() < 256u && (rng() % 100u) < 55u);
        if (doAlloc) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 2048u;
            void* p = heap.alloc({size, align, nullptr});
            if (p == nullptr) {
                continue;
            }
            ++r.allocs;
            r.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial};
            stamp(b);
            live.push_back(b);
        } else {
            const u32 idx = rng() % static_cast<u32>(live.size());
            r.corrupted += verify(live[idx]) ? 0u : 1u;
            heap.free(live[idx].ptr, live[idx].size);
            ++r.frees;
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (const LiveBlock& b : live) {
        r.corrupted += verify(b) ? 0u : 1u;
        heap.free(b.ptr, b.size);
        ++r.frees;
    }
    // Fully coalesced arena: one block spanning (nearly) all of it must be allocatable again.
    void* whole = heap.allocate(kCapacity - 64u, 16u);
    const bool leakFree = heap.usedBytes() == 0u || whole != nullptr;
    const bool coalesced = whole != nullptr;
    if (whole != nullptr) {
        heap.free(whole, kCapacity - 64u);
    }
    report("freelist", r, leakFree && coalesced && heap.usedBytes() == 0u);
    expectTrue(r.allocs + r.frees >= kStressOps, "freelist: 1M alloc/free operations");
    expectTrue(r.misaligned == 0u, "freelist: every block honours the requested alignment");
    expectTrue(r.corrupted == 0u, "freelist: no overlapping/corrupted blocks");
    expectTrue(coalesced && heap.usedBytes() == 0u, "freelist: zero leaks, arena fully coalesces after stress");
}

void testRingStress() {
    std::mt19937 rng(3u);
    constexpr u32 kCapacity = 64u * 1024u;
    fuse::alloc::RingAllocator ring(kCapacity, "stress.ring");
    std::deque<LiveBlock> live;
    StressResult r;
    u32 serial = 0;
    for (u32 op = 0; op < kStressOps; ++op) {
        const bool doAlloc = live.empty() || (rng() % 100u) < 52u;
        if (doAlloc) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 1024u;
            void* p = ring.alloc({size, align, nullptr});
            if (p == nullptr) {
                if (!live.empty()) {
                    r.corrupted += verify(live.front()) ? 0u : 1u;
                    ring.free(live.front().ptr, live.front().size);
                    ++r.frees;
                    live.pop_front();
                }
                continue;
            }
            ++r.allocs;
            r.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial};
            stamp(b);
            live.push_back(b);
        } else {
            r.corrupted += verify(live.front()) ? 0u : 1u;
            ring.free(live.front().ptr, live.front().size);
            ++r.frees;
            live.pop_front();
        }
    }
    while (!live.empty()) {
        r.corrupted += verify(live.front()) ? 0u : 1u;
        ring.free(live.front().ptr, live.front().size);
        ++r.frees;
        live.pop_front();
    }
    const bool leakFree = ring.usedBytes() == 0u && ring.failedAllocations() == ring.stats().failedAllocs &&
                          ring.stats().allocCount == ring.stats().freeCount;
    report("ring", r, leakFree);
    expectTrue(r.allocs + r.frees >= kStressOps, "ring: 1M alloc/free operations");
    expectTrue(r.misaligned == 0u, "ring: every block honours the requested alignment");
    expectTrue(r.corrupted == 0u, "ring: no overlapping/corrupted blocks");
    expectTrue(leakFree, "ring: zero leaks after stress");
}

void testStackStress() {
    std::mt19937 rng(4u);
    constexpr u32 kCapacity = 256u * 1024u;
    fuse::alloc::StackAllocator stack(kCapacity, "stress.stack");
    std::vector<LiveBlock> live;
    std::vector<fuse::alloc::StackMarker> marks;
    StressResult r;
    u32 serial = 0;
    for (u32 op = 0; op < kStressOps; ++op) {
        const bool doAlloc = live.empty() || (rng() % 100u) < 55u;
        if (doAlloc) {
            const u32 align = randomAlignment(rng);
            const u32 size = 1u + rng() % 512u;
            const fuse::alloc::StackMarker mark = stack.pushMark();
            void* p = stack.alloc({size, align, nullptr});
            if (p == nullptr) {
                // Full: unwind everything back to the bottom mark.
                for (const LiveBlock& b : live) {
                    r.corrupted += verify(b) ? 0u : 1u;
                }
                r.frees += live.size();
                live.clear();
                marks.clear();
                stack.popToMark(0u);
                continue;
            }
            ++r.allocs;
            r.misaligned += isAligned(p, align) ? 0u : 1u;
            LiveBlock b{p, size, ++serial};
            stamp(b);
            live.push_back(b);
            marks.push_back(mark);
        } else {
            const LiveBlock b = live.back();
            r.corrupted += verify(b) ? 0u : 1u;
            // LIFO: roll back to the mark taken just before this allocation.
            stack.popToMark(marks.back());
            ++r.frees;
            live.pop_back();
            marks.pop_back();
        }
    }
    while (!live.empty()) {
        r.corrupted += verify(live.back()) ? 0u : 1u;
        stack.popToMark(marks.back());
        ++r.frees;
        live.pop_back();
        marks.pop_back();
    }
    const bool leakFree = stack.pushMark() == 0u && stack.stats().usedBytes == 0u;
    report("stack", r, leakFree);
    expectTrue(r.allocs + r.frees >= kStressOps, "stack: 1M alloc/free operations");
    expectTrue(r.misaligned == 0u, "stack: every block honours the requested alignment");
    expectTrue(r.corrupted == 0u, "stack: no overlapping/corrupted blocks");
    expectTrue(leakFree, "stack: zero leaks after stress (all marks unwound)");
}

void testFrameStress() {
    std::mt19937 rng(5u);
    constexpr u32 kCapacity = 64u * 1024u;
    fuse::alloc::FrameAllocator frame(kCapacity, "stress.frame");
    std::vector<LiveBlock> current;
    std::vector<LiveBlock> previous;
    StressResult r;
    u32 serial = 0;
    for (u32 op = 0; op < kStressOps; ++op) {
        const u32 align = randomAlignment(rng);
        const u32 size = 1u + rng() % 512u;
        void* p = frame.alloc({size, align, nullptr});
        if (p == nullptr || (rng() % 64u) == 0u) {
            // End of frame: previous frame's data must still be intact, then it is recycled.
            for (const LiveBlock& b : previous) {
                r.corrupted += verify(b) ? 0u : 1u;
            }
            r.frees += previous.size();
            previous.swap(current);
            current.clear();
            frame.advanceFrame();
            continue;
        }
        ++r.allocs;
        r.misaligned += isAligned(p, align) ? 0u : 1u;
        LiveBlock b{p, size, ++serial};
        stamp(b);
        current.push_back(b);
    }
    for (const LiveBlock& b : current) {
        r.corrupted += verify(b) ? 0u : 1u;
    }
    for (const LiveBlock& b : previous) {
        r.corrupted += verify(b) ? 0u : 1u;
    }
    r.frees += current.size() + previous.size();
    frame.advanceFrame();
    frame.advanceFrame();
    const bool leakFree = frame.usedBytes() == 0u && frame.previousFrameUsedBytes() == 0u;
    report("frame", r, leakFree);
    expectTrue(r.allocs + r.frees >= kStressOps, "frame: 1M alloc/free operations");
    expectTrue(r.misaligned == 0u, "frame: every block honours the requested alignment");
    expectTrue(r.corrupted == 0u, "frame: allocations survive through the next frame uncorrupted");
    expectTrue(leakFree, "frame: zero bytes retained after two empty frames");
}

void testOverAlignedRequestsFailClosed() {
    fuse::alloc::FrameAllocator frame(4096u);
    fuse::alloc::StackAllocator stack(4096u);
    fuse::alloc::RingAllocator ring(4096u);
    fuse::alloc::FreeListAllocator heap(4096u);
    fuse::alloc::PoolAllocator pool(1024u, 2u);
    const fuse::alloc::AllocInfo huge{16u, 1024u, nullptr};
    const fuse::alloc::AllocInfo odd{16u, 24u, nullptr};
    expectTrue(frame.alloc(huge) == nullptr && frame.alloc(odd) == nullptr,
               "frame rejects alignments it cannot honour");
    expectTrue(stack.alloc(huge) == nullptr && stack.alloc(odd) == nullptr,
               "stack rejects alignments it cannot honour");
    expectTrue(ring.alloc(huge) == nullptr && ring.alloc(odd) == nullptr, "ring rejects alignments it cannot honour");
    expectTrue(heap.alloc(huge) == nullptr && heap.alloc(odd) == nullptr,
               "freelist rejects alignments it cannot honour");
    expectTrue(pool.alloc(huge) == nullptr && pool.alloc(odd) == nullptr, "pool rejects alignments it cannot honour");
}

// ---- assert capture --------------------------------------------------------------------------

struct FatalCapture {
    int count = 0;
};

void onFatal(const fuse::assertion::FatalContext& /*context*/, void* userData) {
    static_cast<FatalCapture*>(userData)->count += 1;
}

constexpr bool kDebugAsserts =
#if defined(FUSE_DEBUG) && FUSE_DEBUG && !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
    true;
#else
    false;
#endif

void testFramePingPongNoUseAfterReset() {
    fuse::alloc::FrameAllocator frame(1024u, "pingpong");
    auto* n0 = frame.allocate<u8>(32u);
    std::memset(n0, 0xAB, 32u);
    expectTrue(frame.isLive(n0), "frame N allocation is live during frame N");

    frame.advanceFrame(); // frame N+1
    auto* n1 = frame.allocate<u8>(32u);
    std::memset(n1, 0xCD, 32u);
    expectTrue(n1 != n0, "frame N+1 uses the other ping-pong buffer");
    expectTrue(frame.isLive(n0), "frame N allocation stays live through frame N+1");
    expectTrue(frame.previousFrameData() == n0 && n0[31] == 0xAB, "frame N bytes intact during frame N+1");

    frame.advanceFrame(); // frame N+2 recycles frame N's buffer
    expectTrue(!frame.isLive(n0), "frame N allocation is dead once frame N+2 starts");
    expectTrue(frame.isLive(n1), "frame N+1 allocation is retained as the previous frame");
    auto* n2 = frame.allocate<u8>(1u);
    expectTrue(n2 == n0, "frame N+2 reuses frame N's buffer (ping-pong)");
    if constexpr (kDebugAsserts) {
        bool poisoned = true;
        for (u32 i = 1; i < 32u; ++i) {
            poisoned = poisoned && n0[i] == fuse::alloc::FrameAllocator::kFreedFramePattern;
        }
        expectTrue(poisoned, "debug: recycled frame bytes are poisoned (use-after-reset is visible)");
    }

    frame.reset();
    expectTrue(!frame.isLive(n2), "reset kills current-frame allocations");
}

void testPoolDoubleFreeDetected() {
    FatalCapture capture;
    fuse::assertion::setFatalHandler(onFatal, &capture);
    fuse::assertion::setSuppressAbortForTests(true);

    fuse::alloc::PoolAllocator pool(64u, 4u, "doublefree");
    void* a = pool.allocate<u8>();
    void* b = pool.allocate<u8>();
    const u32 genLive = pool.generationOf(a);
    expectTrue((genLive & 1u) == 1u && pool.isLive(a), "live block has an odd generation");

    pool.free(a, 64u);
    expectTrue(!pool.isLive(a) && pool.generationOf(a) == genLive + 1u, "free bumps the generation");
    const u32 available = pool.availableBlocks();

    pool.free(a, 64u); // double free
    expectTrue(pool.doubleFreeCount() == 1u, "double free detected via generation counter");
    expectTrue(pool.availableBlocks() == available, "double free does not corrupt the freelist");
    expectTrue(capture.count == (kDebugAsserts ? 1 : 0), "double free asserts in debug only");

    void* c = pool.allocate<u8>();
    void* d = pool.allocate<u8>();
    expectTrue(c != d && c != b && d != b, "no block is handed out twice after a double free");

    fuse::assertion::setSuppressAbortForTests(false);
    fuse::assertion::clearFatalHandler();
}

void testBudgetAssertsOverLimit() {
    FatalCapture capture;
    fuse::assertion::setFatalHandler(onFatal, &capture);
    fuse::assertion::setSuppressAbortForTests(true);

    fuse::alloc::DomainBudget scene("scene", 1024u);
    expectTrue(scene.tryCharge(1000u), "charge under budget succeeds");
    expectTrue(capture.count == 0, "no assert while inside the budget");
    expectTrue(!scene.fits(100u), "fits() probes without asserting");
    expectTrue(capture.count == 0, "fits() never asserts");
    expectTrue(!scene.tryCharge(100u), "over-budget charge fails closed");
    expectTrue(scene.used() == 1000u, "over-budget charge consumes nothing");
    expectTrue(capture.count == (kDebugAsserts ? 1 : 0), "exceeding a domain budget asserts in debug only");

    fuse::assertion::setSuppressAbortForTests(false);
    fuse::assertion::clearFatalHandler();
}

void testHotPathZeroHeapAllocations() {
    auto& sched = fuse::jobs::JobScheduler::instance();
    sched.shutdown();
    sched.initialize(2);

    fuse::alloc::PoolAllocator pool(64u, 64u, "hotpath");
    fuse::jobs::JobCounter counter(0);
    std::atomic<u64> checksum{0};

    struct Payload {
        fuse::alloc::PoolAllocator* pool;
        fuse::jobs::JobCounter* counter;
    };
    auto iteration = [&](u32 i) {
        counter.reset(1);
        Payload payload{&pool, &counter};
        // 16-byte capture: stored inline by std::function (no heap).
        sched.submit([payload]() {
            payload.counter->signal();
        });
        counter.wait();
        void* block = pool.alloc({64u, 16u, nullptr});
        static_cast<u32*>(block)[0] = i;
        checksum.fetch_add(static_cast<u32*>(block)[0], std::memory_order_relaxed);
        pool.free(block, 64u);
    };

    // Warm-up grows queues and creates worker fibers.
    for (u32 i = 0; i < 20'000u; ++i) {
        iteration(i);
    }

    constexpr u32 kIterations = 100'000u;
    const u64 before = g_heapAllocations.load(std::memory_order_acquire);
    for (u32 i = 0; i < kIterations; ++i) {
        iteration(i);
    }
    const u64 heapAllocs = g_heapAllocations.load(std::memory_order_acquire) - before;
    sched.shutdown();

    std::printf("  hot path: %u iterations, %llu heap allocations (%.4f per iteration)\n", kIterations,
                static_cast<unsigned long long>(heapAllocs),
                static_cast<double>(heapAllocs) / static_cast<double>(kIterations));
    expectTrue(heapAllocs == 0u, "submit -> execute -> complete -> pool alloc -> free makes zero heap allocations");
    expectTrue(pool.availableBlocks() == 64u, "hot path returns every pool block");
}

} // namespace

int main() {
    testPoolStress();
    testFreeListStress();
    testRingStress();
    testStackStress();
    testFrameStress();
    testOverAlignedRequestsFailClosed();
    testFramePingPongNoUseAfterReset();
    testPoolDoubleFreeDetected();
    testBudgetAssertsOverLimit();
    testHotPathZeroHeapAllocations();

    if (g_failures == 0) {
        std::printf("fuse_core_b1_memory_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_memory_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
