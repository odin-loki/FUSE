#include <fuse/config.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/jobs/worker_context.hpp>
#include <fuse/platform/fiber.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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

void testCounterWait() {
    fuse::jobs::JobCounter counter(2);
    std::atomic<bool> done{false};

    withScheduler(2, [&] {
        fuse::jobs::JobScheduler::instance().submit([&] {
            counter.signal();
            fuse::jobs::JobScheduler::instance().submit([&] {
                counter.signal();
                done.store(true, std::memory_order_release);
            });
        });
        counter.wait();
    });

    expectTrue(done.load(std::memory_order_acquire), "nested jobs complete before counter wait returns");
    expectTrue(counter.isComplete(), "counter reaches zero");
}

void testCooperativeWorkerWait() {
    if (!fuse::platform::cooperativeFibersAvailable()) {
        std::printf("SKIP: cooperative fibers unavailable on this platform\n");
        return;
    }

    fuse::jobs::JobCounter gate(1);
    std::atomic<bool> waiterResumed{false};
    std::atomic<bool> waiterStarted{false};

    withScheduler(2, [&] {
        auto& sched = fuse::jobs::JobScheduler::instance();
        sched.submit([&] {
            waiterStarted.store(true, std::memory_order_release);
            gate.wait();
            waiterResumed.store(true, std::memory_order_release);
        });

        while (!waiterStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        sched.submit([&] {
            gate.signal();
        });

        gate.wait();
    });

    expectTrue(waiterResumed.load(std::memory_order_acquire), "worker resumed after cooperative wait");
}

void testParallelForMatchesSerial() {
    withScheduler(4, [&] {
        fuse::u32 parallelSum = 0;
        fuse::u32 serialSum = 0;

        fuse::jobs::parallel_for(0u, 256u, 8u, [&parallelSum](fuse::u32 i) {
            parallelSum += i;
        });

        for (fuse::u32 i = 0; i < 256u; ++i) {
            serialSum += i;
        }

        expectEq(parallelSum, serialSum, "parallel_for sum matches serial");
    });
}

void testSingleThreadFallback() {
    withScheduler(0, [&] {
        fuse::u32 sum = 0;
        fuse::jobs::parallel_for(0u, 50u, 5u, [&sum](fuse::u32 i) { sum += i; });
        expectEq(sum, 1225u, "single-thread parallel_for still correct");
        expectTrue(fuse::jobs::JobScheduler::instance().isSingleThreaded(), "scheduler reports single-threaded");
    });
}

} // namespace

int main() {
    testCounterWait();
    testCooperativeWorkerWait();
    testParallelForMatchesSerial();
    testSingleThreadFallback();

    if (g_failures == 0) {
        std::printf("fuse_core job tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core job tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
