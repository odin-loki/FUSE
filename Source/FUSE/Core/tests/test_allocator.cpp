#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/alloc/pool_allocator.hpp>
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
    testGlobalStatsHook();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_allocator: all tests passed\n");
    return EXIT_SUCCESS;
}
