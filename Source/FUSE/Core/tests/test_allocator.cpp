#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/domain_budget.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/alloc/freelist_allocator.hpp>
#include <fuse/alloc/pool_allocator.hpp>
#include <fuse/alloc/ring_allocator.hpp>
#include <fuse/alloc/size_class_allocator.hpp>
#include <fuse/alloc/stack_allocator.hpp>
#include <fuse/assert.hpp>
#include <fuse/core/flat_u64_map.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void resetStatsHook() {
    fuse::alloc::clearGlobalStatsHook();
}

void testFrameAllocatorBumpReset() {
    fuse::alloc::FrameAllocator frame(256u);
    auto* a = frame.allocate<fuse::u8>(32u);
    auto* b = frame.allocate<fuse::u8>(32u);
    expectTrue(a != nullptr && b != nullptr, "frame allocator returns storage");
    frame.reset();
    auto* c = frame.allocate<fuse::u8>(32u);
    expectTrue(c == a, "frame allocator resets bump pointer");
}

void testFrameAllocatorAlignmentAndPeak() {
    fuse::alloc::FrameAllocator frame(128u);
    auto* unaligned = frame.allocate(1u, 16u);
    expectTrue(unaligned != nullptr, "frame allocator honors alignment");
    expectTrue(reinterpret_cast<uintptr_t>(unaligned) % 16u == 0u, "frame pointer is aligned");

    frame.allocate<fuse::u8>(64u);
    expectTrue(frame.peakUsedBytes() >= frame.usedBytes(), "peak tracks high water mark");
    expectTrue(frame.allocationCount() >= 2u, "allocation count increments");
}

void testFrameAllocatorPingPong() {
    fuse::alloc::FrameAllocator frame(64u);
    auto* first = frame.allocate<fuse::u8>(8u);
    std::memset(first, 0xAB, 8u);
    frame.advanceFrame();

    auto* second = frame.allocate<fuse::u8>(8u);
    expectTrue(second != nullptr, "advanceFrame allows allocations on fresh buffer");
    expectTrue(second != first, "ping-pong uses alternate buffer storage");
    expectTrue(frame.previousFrameUsedBytes() >= 8u, "previous frame usage preserved");

    const fuse::u8* previous = frame.previousFrameData();
    expectTrue(previous != nullptr, "previous frame data is readable");
    expectTrue(previous[0] == 0xAB, "previous frame bytes survive one frame");
}

void testFrameAllocatorOomStats() {
    fuse::alloc::FrameAllocator frame(16u);
    expectTrue(frame.allocate<fuse::u8>(32u) == nullptr, "frame allocator returns null on overflow");
    expectTrue(frame.failedAllocations() == 1u, "failed allocation is counted");
}

void testPoolAllocatorFreelist() {
    fuse::alloc::PoolAllocator pool(16u, 4u);
    void* a = pool.allocate<fuse::u8>();
    void* b = pool.allocate<fuse::u8>();
    expectTrue(a != nullptr && b != nullptr && a != b, "pool returns distinct blocks");
    expectTrue(pool.availableBlocks() == 2u, "pool tracks available blocks");

    pool.free(a, 16u);
    expectTrue(pool.availableBlocks() == 3u, "pool free returns block to freelist");

    void* c = pool.allocate<fuse::u8>();
    expectTrue(c == a, "pool reuses freed block");
}

void testPoolAllocatorRejectsOversize() {
    fuse::alloc::PoolAllocator pool(8u, 2u);
    expectTrue(pool.alloc({16u, 1u, nullptr}) == nullptr, "pool rejects oversize request");
    expectTrue(pool.stats().failedAllocs == 1u, "pool records failed alloc");
}

void testStackAllocatorMarkRollback() {
    fuse::alloc::StackAllocator stack(128u);
    const fuse::alloc::StackMarker mark = stack.pushMark();
    auto* a = stack.allocate<fuse::u8>(16u);
    expectTrue(a != nullptr, "stack allocator returns storage");

    stack.popToMark(mark);
    auto* b = stack.allocate<fuse::u8>(16u);
    expectTrue(b == a, "stack rollback restores bump pointer");
}

void testStackAllocatorLifoFree() {
    fuse::alloc::StackAllocator stack(64u);
    auto* a = stack.allocate<fuse::u8>(8u);
    auto* b = stack.allocate<fuse::u8>(8u);
    expectTrue(b != a, "stack grows with allocations");

    stack.free(b, 8u);
    expectTrue(stack.stats().usedBytes == 8u, "stack LIFO free shrinks top allocation");
}

void testRingAllocatorWrap() {
    fuse::alloc::RingAllocator ring(128u);
    auto* a = ring.allocate<fuse::u8>(32u);
    auto* b = ring.allocate<fuse::u8>(32u);
    expectTrue(a != nullptr && b != nullptr && a != b, "ring fills from the head");

    ring.free(a, 32u);
    auto* c = ring.allocate<fuse::u8>(32u);
    expectTrue(c != nullptr, "ring wraps into the freed prefix");
    expectTrue(c == a, "wrapped alloc lands at the start of the buffer");
    expectTrue(ring.usedBytes() > 0u, "ring used tracks live + wrap padding");
    expectTrue(ring.stats().peakUsedBytes >= ring.usedBytes(), "ring peak tracks high water");

    expectTrue(ring.allocate<fuse::u8>(32u) == nullptr, "ring fails when wrap would clobber live data");
    expectTrue(ring.failedAllocations() >= 1u, "ring records wrap-clobber failure");
}

void testRingAllocatorOom() {
    fuse::alloc::RingAllocator ring(32u);
    expectTrue(ring.allocate<fuse::u8>(64u) == nullptr, "ring OOM when request exceeds capacity");
    expectTrue(ring.failedAllocations() == 1u, "ring records OOM");

    auto* a = ring.allocate<fuse::u8>(8u);
    expectTrue(a != nullptr, "smaller alloc still succeeds after OOM");
    expectTrue(ring.allocate<fuse::u8>(32u) == nullptr, "ring rejects alloc that cannot wrap without clobber");
    expectTrue(ring.failedAllocations() == 2u, "ring counts subsequent OOM");
}

void testFreeListReuse() {
    fuse::alloc::FreeListAllocator heap(256u);
    void* a = heap.allocate<fuse::u8>(32u);
    void* b = heap.allocate<fuse::u8>(32u);
    expectTrue(a != nullptr && b != nullptr && a != b, "freelist returns distinct blocks");

    heap.free(a, 32u);
    void* c = heap.allocate<fuse::u8>(32u);
    expectTrue(c == a, "freelist first-fit reuses a freed block");
    expectTrue(heap.stats().freeCount >= 1u, "freelist records free");
}

void testFreeListCoalesce() {
    fuse::alloc::FreeListAllocator heap(256u);
    void* a = heap.allocate<fuse::u8>(32u);
    void* b = heap.allocate<fuse::u8>(32u);
    void* c = heap.allocate<fuse::u8>(32u);
    expectTrue(a != nullptr && b != nullptr && c != nullptr, "freelist allocates three adjacent blocks");

    heap.free(a, 32u);
    heap.free(b, 32u);
    void* d = heap.allocate<fuse::u8>(80u);
    expectTrue(d != nullptr, "freelist coalesces adjacent free blocks");
    expectTrue(d == a, "coalesced region starts at the first freed block");
}

void testFreeListOom() {
    fuse::alloc::FreeListAllocator heap(64u);
    expectTrue(heap.allocate<fuse::u8>(128u) == nullptr, "freelist OOM when request exceeds arena");
    expectTrue(heap.failedAllocations() == 1u, "freelist records failed alloc");

    void* a = heap.allocate<fuse::u8>(48u);
    expectTrue(a != nullptr, "freelist still serves a fitting request after OOM");
    expectTrue(heap.allocate<fuse::u8>(48u) == nullptr, "freelist rejects a second block that does not fit");
    expectTrue(heap.failedAllocations() == 2u, "freelist counts in-arena OOM");
}

void testDomainBudgetReject() {
    // Over-budget charges raise FUSE_ASSERT in debug builds; keep the process alive to check the
    // fail-closed bookkeeping (debug assert coverage lives in test_b1_memory_gates.cpp).
    fuse::assertion::setSuppressAbortForTests(true);
    fuse::alloc::DomainBudget core("core", 32u);
    fuse::alloc::DomainBudget frame("frame", 64u);
    fuse::alloc::DomainBudget scene("scene", 64u);

    expectTrue(core.tryCharge(16u), "core charge under cap");
    expectTrue(frame.tryCharge(40u), "frame charge under cap");
    expectTrue(scene.tryCharge(40u), "scene charge under cap");
    expectTrue(scene.used() == 40u, "scene used tracks charge");
    expectTrue(!scene.tryCharge(32u), "scene fail closed when over cap");
    expectTrue(scene.used() == 40u, "failed charge does not consume");
    expectTrue(scene.failed() == 1u, "failed charge is counted");
    expectTrue(scene.stats().failedAllocs == 1u, "budget stats surface failed charges");

    scene.release(40u);
    expectTrue(scene.used() == 0u, "release returns budget");
    expectTrue(scene.tryCharge(64u), "exact cap succeeds");
    expectTrue(scene.peak() == 64u, "peak tracks high water");
    expectTrue(!scene.tryCharge(1u), "charge at cap is rejected");
    fuse::assertion::setSuppressAbortForTests(false);
}

void testIAllocatorHierarchy() {
    fuse::alloc::RingAllocator ring(64u, "ring");
    fuse::alloc::FreeListAllocator heap(64u, "freelist");
    fuse::alloc::IAllocator* allocators[] = {&ring, &heap};
    for (fuse::alloc::IAllocator* allocator : allocators) {
        void* p = allocator->alloc({8u, 8u, "p1"});
        expectTrue(p != nullptr, "IAllocator alloc succeeds");
        expectTrue(allocator->stats().allocCount >= 1u, "IAllocator stats track alloc");
        expectTrue(allocator->name() != nullptr, "IAllocator reports a name");
        allocator->free(p, 8u);
        allocator->reset();
        expectTrue(allocator->stats().usedBytes == 0u, "IAllocator reset clears used");
    }
}

void testGlobalStatsHook() {
    resetStatsHook();

    struct HookState {
        std::string lastName;
        fuse::alloc::AllocStats lastStats{};
        fuse::u32 callCount = 0;
    } state;

    fuse::alloc::setGlobalStatsHook(
        [](const char* allocatorName, const fuse::alloc::AllocStats& stats, void* userData) {
            auto* hookState = static_cast<HookState*>(userData);
            hookState->lastName = allocatorName != nullptr ? allocatorName : "";
            hookState->lastStats = stats;
            hookState->callCount += 1;
        },
        &state);

    fuse::alloc::FrameAllocator frame(32u, "test_frame");
    frame.allocate<fuse::u8>(4u);
    expectTrue(state.callCount >= 1u, "stats hook fires on allocation");
    expectTrue(state.lastName == "test_frame", "stats hook receives allocator name");
    expectTrue(state.lastStats.allocCount >= 1u, "stats hook receives alloc count");
}

void testSizeClassAllocatorClasses() {
    using fuse::alloc::SizeClassAllocator;
    // Every size maps to the smallest class that holds it; classes are 16-byte multiples.
    fuse::u32 previous = 0;
    for (fuse::usize size = 1; size <= SizeClassAllocator::kMaxClassBytes; ++size) {
        const fuse::u32 cls = SizeClassAllocator::classIndex(size);
        const fuse::usize bytes = SizeClassAllocator::classBytes(cls);
        if (cls >= SizeClassAllocator::kClassCount || bytes < size || bytes % 16u != 0u || cls < previous ||
            (cls > 0u && SizeClassAllocator::classBytes(cls - 1u) >= size)) {
            expectTrue(false, "size class mapping is tight and monotonic");
            return;
        }
        previous = cls;
    }
    expectTrue(SizeClassAllocator::classIndex(SizeClassAllocator::kMaxClassBytes + 1u) ==
                   SizeClassAllocator::kClassCount,
               "requests above the largest class are oversize");
}

void testSizeClassAllocatorRecyclesWithoutSystemAllocs() {
    fuse::alloc::SizeClassAllocator heap({"test_sizeclass", 64u * 1024u, 0u, false});
    void* blocks[64] = {};
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 64; ++i) {
            blocks[i] = heap.allocate(static_cast<fuse::usize>(8 + i * 37));
            expectTrue(blocks[i] != nullptr && reinterpret_cast<uintptr_t>(blocks[i]) % 16u == 0u,
                       "size-class blocks are 16-byte aligned");
            std::memset(blocks[i], i, static_cast<fuse::usize>(8 + i * 37));
        }
        for (int i = 0; i < 64; ++i) {
            heap.deallocate(blocks[i], static_cast<fuse::usize>(8 + i * 37));
        }
    }
    const fuse::u64 warm = heap.systemAllocations();
    for (int i = 0; i < 64; ++i) {
        blocks[i] = heap.allocate(static_cast<fuse::usize>(8 + i * 37));
    }
    for (int i = 0; i < 64; ++i) {
        heap.deallocate(blocks[i], static_cast<fuse::usize>(8 + i * 37));
    }
    expectTrue(heap.systemAllocations() == warm, "warm size-class pool serves a repeat workload without system allocs");
    expectTrue(heap.stats().usedBytes == 0u, "all blocks returned");

    // realloc keeps the pointer inside a class and preserves contents across classes.
    auto* p = static_cast<unsigned char*>(heap.reallocate(nullptr, 0u, 20u));
    std::memset(p, 0x5A, 20u);
    expectTrue(heap.reallocate(p, 20u, 30u) == p, "realloc within a size class keeps the block");
    auto* q = static_cast<unsigned char*>(heap.reallocate(p, 30u, 5000u));
    expectTrue(q != nullptr && q[0] == 0x5A && q[19] == 0x5A, "realloc across classes copies the contents");
    auto* big = static_cast<unsigned char*>(heap.reallocate(q, 5000u, 100000u));
    expectTrue(big != nullptr && big[19] == 0x5A, "realloc into an oversize block copies the contents");
    expectTrue(heap.reallocate(big, 100000u, 0u) == nullptr, "realloc to zero frees");

    void* unsized = heap.allocateUnsized(777u);
    expectTrue(unsized != nullptr && reinterpret_cast<uintptr_t>(unsized) % 16u == 0u, "unsized block aligned");
    heap.deallocateUnsized(unsized);
    expectTrue(heap.stats().usedBytes == 0u, "unsized block returned");
}

void testFlatU64Map() {
    fuse::FlatU64Map<fuse::u32> map;
    for (fuse::u32 round = 0; round < 3; ++round) {
        map.clear();
        for (fuse::u32 i = 0; i < 100; ++i) {
            const auto [value, inserted] = map.try_emplace((static_cast<fuse::u64>(i) << 32u) | (i * 7u), i);
            expectTrue(inserted && *value == i, "flat map inserts new keys");
        }
        expectTrue(!map.try_emplace(0u, 99u).second, "flat map keeps the first value of a key");
        expectTrue(map.size() == 100u, "flat map size");
    }
    expectTrue(map.find((5ull << 32u) | 35u) != nullptr && *map.find((5ull << 32u) | 35u) == 5u, "flat map find");
    expectTrue(map.find(12345u) == nullptr, "flat map miss");
    fuse::u32 visited = 0;
    fuse::u32 expected = 0;
    bool ordered = true;
    map.for_each([&](fuse::u64, fuse::u32 value) {
        ordered = ordered && value == expected++;
        ++visited;
    });
    expectTrue(visited == 100u && ordered, "flat map iterates in insertion order");
}

} // namespace

int main() {
    testFrameAllocatorBumpReset();
    testFrameAllocatorAlignmentAndPeak();
    testFrameAllocatorPingPong();
    testFrameAllocatorOomStats();
    testPoolAllocatorFreelist();
    testPoolAllocatorRejectsOversize();
    testStackAllocatorMarkRollback();
    testStackAllocatorLifoFree();
    testRingAllocatorWrap();
    testRingAllocatorOom();
    testFreeListReuse();
    testFreeListCoalesce();
    testFreeListOom();
    testDomainBudgetReject();
    testIAllocatorHierarchy();
    testGlobalStatsHook();
    testSizeClassAllocatorClasses();
    testSizeClassAllocatorRecyclesWithoutSystemAllocs();
    testFlatU64Map();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_allocator: all tests passed\n");
    return EXIT_SUCCESS;
}
