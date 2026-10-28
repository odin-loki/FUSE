#include <fuse/frame/frame_barrier.hpp>
#include <fuse/jobs/barrier_count.hpp>
#include <fuse/jobs/barrier_generation.hpp>
#include <fuse/math/spring_arm.hpp>

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

void testCriticallyDampedSpring() {
    const auto expectNear = [](fuse::f32 value, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
        const fuse::f32 delta = value - expected;
        if (!(delta <= epsilon && delta >= -epsilon)) {
            std::fprintf(stderr, "FAIL: %s (expected %g, got %g)\n", message, static_cast<double>(expected),
                         static_cast<double>(value));
            ++g_failures;
        }
    };

    constexpr fuse::f32 kTarget = 5.f;
    const fuse::math::Spring1D held{kTarget, 0.f};
    const fuse::math::Spring1D heldStep = fuse::math::step_spring(held, kTarget, 8.f, 2.f);
    expectTrue(heldStep.value == kTarget, "spring held at the target keeps its value");
    expectTrue(heldStep.velocity == 0.f, "spring held at the target keeps zero velocity");

    const fuse::math::Spring1D displaced{-2.f, 0.f};
    const fuse::math::Spring1D settled = fuse::math::step_spring(displaced, kTarget, 8.f, 2.f);
    expectNear(settled.value, kTarget, 1e-3f, "displaced critically damped spring is within 1e-3 of the target");
    expectNear(settled.velocity, 0.f, 1e-3f, "displaced critically damped spring velocity is near zero");
}

void testBarrierCountOk() {
    expectTrue(fuse::jobs::barrier_count_ok(1), "barrier_count_ok(1) is true");
    expectTrue(fuse::jobs::barrier_count_ok(64), "barrier_count_ok(64) is true");
    expectTrue(!fuse::jobs::barrier_count_ok(0), "barrier_count_ok(0) is false");
    expectTrue(!fuse::jobs::barrier_count_ok(65), "barrier_count_ok(65) is false");
}

void testBarrierGenerationOk() {
    expectTrue(fuse::jobs::barrier_generation_ok(1ull), "barrier_generation_ok(1) is true");
    expectTrue(fuse::jobs::barrier_generation_ok(1024ull), "barrier_generation_ok(1024) is true");
    expectTrue(!fuse::jobs::barrier_generation_ok(0ull), "barrier_generation_ok(0) is false");
    expectTrue(!fuse::jobs::barrier_generation_ok(1025ull), "barrier_generation_ok(1025) is false");
}

} // namespace

int main() {
    testBarrierCountOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core frame barrier tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testBarrierGenerationOk();
    testBeginTickResetsState();
    testSignalMarksComplete();
    testWaitForTickCompleteIsNoOpInV1();
    testWaitBeforeSignalLeavesIncomplete();
    testWaitAfterSignalPreservesComplete();
    testMultipleWaitCallsAreIdempotent();
    testWaitDoesNotAdvanceFrameIndex();
    testBeginTickAfterWaitResetsPreviousFrame();
    testSequentialFramesAdvanceIndex();
    testCriticallyDampedSpring();

    if (g_failures == 0) {
        std::printf("fuse_core frame barrier tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core frame barrier tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
