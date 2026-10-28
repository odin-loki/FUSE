#include <fuse/config.hpp>
#include <fuse/math/camera_shake.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_queue.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/jobs/single_worker.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/jobs/worker_pin.hpp>
#include <fuse/platform/fiber.hpp>
#include <fuse/platform/power.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testJobQueueOk() {
    expectTrue(fuse::jobs::job_queue_ok(0), "job_queue_ok(0) is true");
    expectTrue(fuse::jobs::job_queue_ok(1024), "job_queue_ok(1024) is true");
    expectTrue(!fuse::jobs::job_queue_ok(-1), "job_queue_ok(-1) is false");
    expectTrue(!fuse::jobs::job_queue_ok(1025), "job_queue_ok(1025) is false");
}

} // namespace

#if !FUSE_JOBS_SINGLE_THREAD
int main() {
    testJobQueueOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core job single-thread tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "FAIL: test_jobs_single_thread must be built with FUSE_JOBS_SINGLE_THREAD=ON\n");
    return EXIT_FAILURE;
}
#else

namespace {

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

// Default CI keeps fuse_core multi-thread; this test executable alone defines
// FUSE_JOBS_SINGLE_THREAD. Probe the linked library before asserting clamps.
bool fuseCoreCompiledSingleThread() {
    fuse::jobs::WorkerCountParams desktop;
    desktop.usableCores = 16;
    desktop.performanceCores = 16;
    desktop.mobileProfile = false;
    desktop.powerState = fuse::platform::PowerState::Normal;
    return fuse::jobs::computeWorkerCount(desktop) == 0u;
}

void testWorkerPinOk() {
    expectTrue(fuse::jobs::worker_pin_ok(0), "worker_pin_ok(0) is true");
    expectTrue(fuse::jobs::worker_pin_ok(63), "worker_pin_ok(63) is true");
    expectTrue(!fuse::jobs::worker_pin_ok(-1), "worker_pin_ok(-1) is false");
    expectTrue(!fuse::jobs::worker_pin_ok(64), "worker_pin_ok(64) is false");
}

void testSingleWorkerOk() {
    expectTrue(fuse::jobs::single_worker_ok(1), "single_worker_ok(1) is true");
    expectTrue(!fuse::jobs::single_worker_ok(0), "single_worker_ok(0) is false");
    expectTrue(!fuse::jobs::single_worker_ok(2), "single_worker_ok(2) is false");
}

void testMacroEnabled() {
    expectTrue(FUSE_JOBS_SINGLE_THREAD == 1, "FUSE_JOBS_SINGLE_THREAD compile flag is set");
}

void testComputeWorkerCountAlwaysZero() {
    if (!fuseCoreCompiledSingleThread()) {
        std::printf("SKIP: computeWorkerCount clamp requires fuse_core FUSE_JOBS_SINGLE_THREAD\n");
        return;
    }

    fuse::jobs::WorkerCountParams desktop;
    desktop.usableCores = 16;
    desktop.performanceCores = 16;
    desktop.mobileProfile = false;
    desktop.powerState = fuse::platform::PowerState::Normal;
    expectEq(fuse::jobs::computeWorkerCount(desktop), 0u, "desktop profile yields 0 workers");

    fuse::jobs::WorkerCountParams mobile;
    mobile.usableCores = 8;
    mobile.performanceCores = 4;
    mobile.mobileProfile = true;
    mobile.powerState = fuse::platform::PowerState::Normal;
    expectEq(fuse::jobs::computeWorkerCount(mobile), 0u, "mobile profile yields 0 workers");

    fuse::jobs::WorkerCountParams background;
    background.usableCores = 8;
    background.mobileProfile = false;
    background.powerState = fuse::platform::PowerState::Background;
    expectEq(fuse::jobs::computeWorkerCount(background), 0u, "background profile yields 0 workers");

    fuse::jobs::WorkerCountParams thermal;
    thermal.usableCores = 8;
    thermal.mobileProfile = false;
    thermal.powerState = fuse::platform::PowerState::Thermal;
    expectEq(fuse::jobs::computeWorkerCount(thermal), 0u, "thermal profile yields 0 workers");
}

void testSchedulerIgnoresRequestedWorkers() {
    // Compile-time clamp is only in ST fuse_core; otherwise exercise runtime initialize(0).
    const fuse::u32 requested = fuseCoreCompiledSingleThread() ? 8u : 0u;
    withScheduler(requested, [&] {
        auto& scheduler = fuse::jobs::JobScheduler::instance();
        expectEq(scheduler.workerCount(), 0u, "initialize results in 0 workers");
        expectTrue(scheduler.isSingleThreaded(), "scheduler reports single-threaded mode");
    });
}

void testSubmitRunsInline() {
    withScheduler(0, [&] {
        std::atomic<int> phase{0};
        fuse::jobs::JobScheduler::instance().submit([&] {
            expectEq(static_cast<fuse::u32>(phase.load(std::memory_order_relaxed)), 0u,
                     "submit runs before caller continues");
            phase.store(1, std::memory_order_release);
        });
        expectEq(static_cast<fuse::u32>(phase.load(std::memory_order_acquire)), 1u,
                 "submit completes inline on caller thread");
    });
}

void testNestedCounterWait() {
    fuse::jobs::JobCounter counter(2);
    std::atomic<bool> nestedDone{false};

    withScheduler(0, [&] {
        auto& scheduler = fuse::jobs::JobScheduler::instance();
        scheduler.submit([&] {
            counter.signal();
            scheduler.submit([&] {
                counter.signal();
                nestedDone.store(true, std::memory_order_release);
            });
        });
        counter.wait();
    });

    expectTrue(nestedDone.load(std::memory_order_acquire), "nested single-thread jobs complete");
    expectTrue(counter.isComplete(), "counter reaches zero");
}

void testParallelForParity() {
    withScheduler(0, [&] {
        fuse::u32 parallelSum = 0;
        fuse::u32 serialSum = 0;

        fuse::jobs::parallel_for(0u, 512u, 17u, [&parallelSum](fuse::u32 i) { parallelSum += i; });
        for (fuse::u32 i = 0; i < 512u; ++i) {
            serialSum += i;
        }

        expectEq(parallelSum, serialSum, "single-thread parallel_for matches serial sum");
    });
}

void testNestedParallelForParity() {
    constexpr fuse::u32 outer = 20u;
    constexpr fuse::u32 inner = 24u;
    std::vector<fuse::u32> grid(outer * inner, 0u);

    withScheduler(0, [&] {
        fuse::jobs::parallel_for(0u, outer, 4u, [&](fuse::u32 o) {
            fuse::jobs::parallel_for(0u, inner, 3u, [&](fuse::u32 i) {
                grid[o * inner + i] = o * inner + i;
            });
        });
    });

    for (fuse::u32 o = 0; o < outer; ++o) {
        for (fuse::u32 i = 0; i < inner; ++i) {
            const fuse::u32 idx = o * inner + i;
            expectEq(grid[idx], idx, "single-thread nested parallel_for fills every cell");
        }
    }
}

void testNestedParallelForVisitCount() {
    constexpr fuse::u32 outer = 16u;
    constexpr fuse::u32 inner = 32u;
    std::atomic<fuse::u32> visitCount{0};

    withScheduler(0, [&] {
        fuse::jobs::parallel_for(0u, outer, outer, [&](fuse::u32 /*o*/) {
            fuse::jobs::parallel_for(0u, inner, 4u, [&](fuse::u32 /*i*/) {
                visitCount.fetch_add(1u, std::memory_order_relaxed);
            });
        });
    });

    expectEq(visitCount.load(std::memory_order_relaxed), outer * inner,
             "single-thread nested parallel_for visits every index");
}

void testNestedParallelForStressVisitCoverage() {
    constexpr fuse::u32 outerCount = 24u;
    constexpr fuse::u32 innerCount = 32u;
    constexpr fuse::u32 iterations = 8u;
    const fuse::u32 outerGrains[] = {1u, 4u, outerCount};
    const fuse::u32 innerGrains[] = {4u, 8u, 16u};

    withScheduler(0, [&] {
        for (fuse::u32 outerGrain : outerGrains) {
            for (fuse::u32 innerGrain : innerGrains) {
                for (fuse::u32 iteration = 0; iteration < iterations; ++iteration) {
                    std::vector<std::atomic<bool>> visited(outerCount * innerCount);
                    for (auto& slot : visited) {
                        slot.store(false, std::memory_order_relaxed);
                    }

                    fuse::jobs::parallel_for(0u, outerCount, outerGrain, [&](fuse::u32 o) {
                        fuse::jobs::parallel_for(0u, innerCount, innerGrain, [&](fuse::u32 i) {
                            const fuse::u32 idx = o * innerCount + i;
                            visited[idx].store(true, std::memory_order_relaxed);
                        });
                    });

                    fuse::u32 visitedCount = 0;
                    for (fuse::u32 idx = 0; idx < outerCount * innerCount; ++idx) {
                        if (visited[idx].load(std::memory_order_relaxed)) {
                            ++visitedCount;
                        }
                    }

                    if (visitedCount != outerCount * innerCount) {
                        std::fprintf(stderr,
                                     "FAIL: nested stress visit coverage outerGrain=%u innerGrain=%u iter=%u "
                                     "(expected %u, got %u)\n",
                                     outerGrain,
                                     innerGrain,
                                     iteration,
                                     outerCount * innerCount,
                                     visitedCount);
                        ++g_failures;
                        return;
                    }
                }
            }
        }
    });
}

void testEmscriptenStubProfileWhenApplicable() {
#if defined(__EMSCRIPTEN__)
    expectEq(fuse::platform::fiberBackendName(), "stub", "Emscripten uses fiber stub backend");
    expectTrue(!fuse::platform::cooperativeFibersAvailable(),
               "Emscripten stub profile has no cooperative fibers");
#else
    expectTrue(fuse::platform::fiberBackendName() != nullptr,
               "fiber backend name available on native targets");
#endif
}

void testDeterministicCameraShake() {
    const auto close = [](fuse::f32 actual, fuse::f32 expected) {
        const fuse::f32 delta = actual - expected;
        return delta <= 1e-5f && delta >= -1e-5f;
    };

    fuse::math::ShakeState state{};
    fuse::math::add_trauma(state, 0.25f);
    fuse::math::add_trauma(state, 0.25f);
    expectTrue(close(state.trauma, 0.5f), "add_trauma accumulates");
    fuse::math::add_trauma(state, 10.f);
    expectTrue(state.trauma == 1.f, "add_trauma clamps to 1");
    fuse::math::add_trauma(state, -5.f);
    expectTrue(state.trauma == 0.f, "add_trauma clamps to 0");

    state.trauma = 0.4f;
    const fuse::f32 nan = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::f32 inf = std::numeric_limits<fuse::f32>::infinity();
    fuse::math::add_trauma(state, nan);
    expectTrue(state.trauma == 0.4f, "non-finite trauma amount adds 0");
    fuse::math::add_trauma(state, inf);
    expectTrue(state.trauma == 0.4f, "infinite trauma amount adds 0");
    fuse::math::add_trauma(state, -inf);
    expectTrue(state.trauma == 0.4f, "negative infinite trauma amount adds 0");

    state.trauma = 0.5f;
    state.frame = 9u;
    fuse::math::decay_trauma(state, 0.25f, 1.f);
    expectTrue(state.trauma == 0.25f, "decay_trauma moves trauma toward 0");
    expectTrue(state.frame == 9u, "decay_trauma leaves the frame unchanged");
    fuse::math::decay_trauma(state, 0.f, 1.f);
    expectTrue(state.trauma == 0.25f, "dt <= 0 leaves trauma unchanged");
    fuse::math::decay_trauma(state, -1.f, 4.f);
    expectTrue(state.trauma == 0.25f, "negative dt leaves trauma unchanged");

    state.trauma = 1.25f;
    fuse::math::decay_trauma(state, 0.f, 1.f);
    expectTrue(state.trauma == 1.25f, "non-positive dt does not clamp");
    fuse::math::decay_trauma(state, 0.1f, 1.f);
    expectTrue(state.trauma == 1.f, "decay result clamps to 1");
    state.trauma = 0.2f;
    fuse::math::decay_trauma(state, 1.f, 1.f);
    expectTrue(state.trauma == 0.f, "decay result clamps to 0");

    state.trauma = 0.f;
    state.frame = 4u;
    const fuse::math::Vec3 zero = fuse::math::shake_offset(state);
    expectTrue(zero.x == 0.f && zero.y == 0.f && zero.z == 0.f, "trauma 0 yields a zero offset");

    // offset = trauma^2 * direction(frame), direction components in [-1, 1].
    state.trauma = 1.f;
    state.frame = 11u;
    const fuse::math::Vec3 full = fuse::math::shake_offset(state);
    const fuse::math::Vec3 fullAgain = fuse::math::shake_offset(state);
    expectTrue(full.x == fullAgain.x && full.y == fullAgain.y && full.z == fullAgain.z,
               "the same state returns the same offset");
    expectTrue(full.length() > 0.f, "unit trauma produces a nonzero offset");
    expectTrue(full.x >= -1.f && full.x <= 1.f && full.y >= -1.f && full.y <= 1.f && full.z >= -1.f &&
                   full.z <= 1.f,
               "unit trauma keeps each axis inside [-1, 1]");

    state.trauma = 0.5f;
    const fuse::math::Vec3 half = fuse::math::shake_offset(state);
    expectTrue(close(half.x, full.x * 0.25f) && close(half.y, full.y * 0.25f) && close(half.z, full.z * 0.25f),
               "offset magnitude scales with trauma^2");

    fuse::math::ShakeState repeat{};
    repeat.trauma = 0.5f;
    repeat.frame = 11u;
    const fuse::math::Vec3 repeatOffset = fuse::math::shake_offset(repeat);
    expectTrue(repeatOffset.x == half.x && repeatOffset.y == half.y && repeatOffset.z == half.z,
               "equal states return the same offset");

    for (fuse::u32 frame = 0u; frame < 64u; ++frame) {
        fuse::math::ShakeState current{};
        current.trauma = 1.f;
        current.frame = frame;
        fuse::math::ShakeState next = current;
        next.frame = frame + 1u;
        const fuse::math::Vec3 a = fuse::math::shake_offset(current);
        const fuse::math::Vec3 b = fuse::math::shake_offset(next);
        expectTrue(a.x != b.x || a.y != b.y || a.z != b.z, "successive frames differ");
    }
}

} // namespace

int main() {
    testJobQueueOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core job single-thread tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testWorkerPinOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core job single-thread tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testSingleWorkerOk();
    testMacroEnabled();
    testComputeWorkerCountAlwaysZero();
    testSchedulerIgnoresRequestedWorkers();
    testSubmitRunsInline();
    testNestedCounterWait();
    testParallelForParity();
    testNestedParallelForParity();
    testNestedParallelForVisitCount();
    testNestedParallelForStressVisitCoverage();
    testEmscriptenStubProfileWhenApplicable();
    testDeterministicCameraShake();

    if (g_failures == 0) {
        std::printf("fuse_core job single-thread tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core job single-thread tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

#endif
