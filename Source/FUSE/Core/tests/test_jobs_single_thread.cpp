#include <fuse/config.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/platform/fiber.hpp>
#include <fuse/platform/power.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if !FUSE_JOBS_SINGLE_THREAD
int main() {
    std::fprintf(stderr, "FAIL: test_jobs_single_thread must be built with FUSE_JOBS_SINGLE_THREAD=ON\n");
    return EXIT_FAILURE;
}
#else

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

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

void testMacroEnabled() {
    expectTrue(FUSE_JOBS_SINGLE_THREAD == 1, "FUSE_JOBS_SINGLE_THREAD compile flag is set");
}

void testComputeWorkerCountAlwaysZero() {
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
    withScheduler(8, [&] {
        auto& scheduler = fuse::jobs::JobScheduler::instance();
        expectEq(scheduler.workerCount(), 0u, "initialize clamps worker count to 0");
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

} // namespace

int main() {
    testMacroEnabled();
    testComputeWorkerCountAlwaysZero();
    testSchedulerIgnoresRequestedWorkers();
    testSubmitRunsInline();
    testNestedCounterWait();
    testParallelForParity();
    testEmscriptenStubProfileWhenApplicable();

    if (g_failures == 0) {
        std::printf("fuse_core job single-thread tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core job single-thread tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

#endif
