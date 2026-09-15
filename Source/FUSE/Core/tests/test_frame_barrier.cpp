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

void testWaitBeforeSignalLeavesIncomplete() {
    fuse::frame::FrameBarrier barrier;
    barrier.beginTick(11u);
    barrier.waitForTickComplete();
    expectTrue(!barrier.tickJobsComplete(), "wait before signal leaves tick incomplete");
    expectEq(barrier.frameIndex(), 11u, "wait before signal preserves frame index");
}

void testWaitAfterSignalPreservesComplete() {
    fuse::frame::FrameBarrier barrier;
    barrier.beginTick(5u);
    barrier.signalTickJobsComplete();
    expectTrue(barrier.tickJobsComplete(), "signal marks complete before wait");

    barrier.waitForTickComplete();
    expectTrue(barrier.tickJobsComplete(), "wait after signal keeps tick complete");
    expectEq(barrier.frameIndex(), 5u, "wait after signal preserves frame index");
}

void testMultipleWaitCallsAreIdempotent() {
    fuse::frame::FrameBarrier barrier;
    barrier.beginTick(2u);
    barrier.signalTickJobsComplete();

    for (int i = 0; i < 4; ++i) {
        barrier.waitForTickComplete();
    }

    expectTrue(barrier.tickJobsComplete(), "repeated wait calls stay complete");
    expectEq(barrier.frameIndex(), 2u, "repeated wait calls preserve frame index");
}

void testWaitDoesNotAdvanceFrameIndex() {
    fuse::frame::FrameBarrier barrier;

    barrier.beginTick(99u);
    barrier.waitForTickComplete();
    barrier.signalTickJobsComplete();
    barrier.waitForTickComplete();

    expectEq(barrier.frameIndex(), 99u, "wait/signal cycle does not bump frame index");
}

void testBeginTickAfterWaitResetsPreviousFrame() {
    fuse::frame::FrameBarrier barrier;

    barrier.beginTick(1u);
    barrier.signalTickJobsComplete();
    barrier.waitForTickComplete();

    barrier.beginTick(2u);
    expectEq(barrier.frameIndex(), 2u, "second beginTick stores new frame index");
    expectTrue(!barrier.tickJobsComplete(), "second beginTick clears completion after prior wait");
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
    testWaitBeforeSignalLeavesIncomplete();
    testWaitAfterSignalPreservesComplete();
    testMultipleWaitCallsAreIdempotent();
    testWaitDoesNotAdvanceFrameIndex();
    testBeginTickAfterWaitResetsPreviousFrame();
    testSequentialFramesAdvanceIndex();

    if (g_failures == 0) {
        std::printf("fuse_core frame barrier tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core frame barrier tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
