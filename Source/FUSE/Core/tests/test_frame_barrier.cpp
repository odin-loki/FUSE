#include <fuse/frame/frame_barrier.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void testBeginTickResetsState() {
    fuse::frame::FrameBarrier barrier;
    barrier.signalTickJobsComplete();
    expectTrue(barrier.tickJobsComplete(), "pre-seeded complete flag");

    barrier.beginTick(42u);
    expectEq(barrier.frameIndex(), 42u, "beginTick stores frame index");
    expectTrue(!barrier.tickJobsComplete(), "beginTick clears tickJobsComplete");
}

void testSignalMarksComplete() {
    fuse::frame::FrameBarrier barrier;
    barrier.beginTick(1u);
    expectTrue(!barrier.tickJobsComplete(), "tick starts incomplete");

    barrier.signalTickJobsComplete();
    expectTrue(barrier.tickJobsComplete(), "signal marks tick jobs complete");
}

void testWaitForTickCompleteIsNoOpInV1() {
    fuse::frame::FrameBarrier barrier;
    barrier.beginTick(7u);
    barrier.waitForTickComplete();
    expectTrue(!barrier.tickJobsComplete(), "waitForTickComplete does not auto-signal in v1");
}

void testSequentialFramesAdvanceIndex() {
    fuse::frame::FrameBarrier barrier;

    for (fuse::u32 frame = 0; frame < 4u; ++frame) {
        barrier.beginTick(frame);
        expectEq(barrier.frameIndex(), frame, "frame index tracks beginTick");
        barrier.signalTickJobsComplete();
        expectTrue(barrier.tickJobsComplete(), "each frame signals completion");
    }
}

} // namespace

int main() {
    testBeginTickResetsState();
    testSignalMarksComplete();
    testWaitForTickCompleteIsNoOpInV1();
    testSequentialFramesAdvanceIndex();

    if (g_failures == 0) {
        std::printf("fuse_core frame barrier tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core frame barrier tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
