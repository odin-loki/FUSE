#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/domain_budget.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/alloc/freelist_allocator.hpp>
#include <fuse/alloc/pool_allocator.hpp>
#include <fuse/alloc/ring_allocator.hpp>
#include <fuse/alloc/stack_allocator.hpp>

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

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_allocator: all tests passed\n");
    return EXIT_SUCCESS;
}
