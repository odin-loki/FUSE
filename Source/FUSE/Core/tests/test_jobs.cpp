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

fuse::u32 serialParallelForChecksum(fuse::u32 begin, fuse::u32 end, fuse::u32 grainSize) {
    fuse::u32 checksum = 0;
    if (grainSize == 0) {
        grainSize = 1;
    }
    for (fuse::u32 chunk = begin; chunk < end; chunk += grainSize) {
        const fuse::u32 chunkEnd = (chunk + grainSize < end) ? (chunk + grainSize) : end;
        for (fuse::u32 i = chunk; i < chunkEnd; ++i) {
            checksum += i * 3u + (i % 5u);
        }
    }
    return checksum;
}

} // namespace

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

        while (!waiterResumed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    expectTrue(waiterResumed.load(std::memory_order_acquire), "worker resumed after cooperative wait");
}

void testParallelForMatchesSerial() {
    withScheduler(4, [&] {
        std::atomic<fuse::u32> parallelSum{0};
        fuse::u32 serialSum = 0;

        fuse::jobs::parallel_for(0u, 256u, 8u, [&parallelSum](fuse::u32 i) {
            parallelSum.fetch_add(i, std::memory_order_relaxed);
        });

        for (fuse::u32 i = 0; i < 256u; ++i) {
            serialSum += i;
        }

        expectEq(parallelSum.load(std::memory_order_relaxed), serialSum, "parallel_for sum matches serial");
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

void testParallelSerialFallbackParity() {
    constexpr fuse::u32 count = 256u;
    constexpr fuse::u32 grain = 11u;
    const fuse::u32 expected = serialParallelForChecksum(0u, count, grain);

    std::atomic<fuse::u32> parallelChecksum{0};
    withScheduler(4, [&] {
        fuse::jobs::parallel_for(0u, count, grain, [&parallelChecksum](fuse::u32 i) {
            parallelChecksum.fetch_add(i * 3u + (i % 5u), std::memory_order_relaxed);
        });
    });

    fuse::u32 fallbackChecksum = 0;
    withScheduler(0, [&] {
        fuse::jobs::parallel_for(0u, count, grain, [&fallbackChecksum](fuse::u32 i) {
            fallbackChecksum += i * 3u + (i % 5u);
        });
    });

    expectEq(parallelChecksum.load(std::memory_order_relaxed), expected,
             "multi-worker parallel_for matches serial reference");
    expectEq(fallbackChecksum, expected, "single-thread fallback matches serial reference");
}

void testNestedParallelForParity() {
    constexpr fuse::u32 outer = 24u;
    constexpr fuse::u32 inner = 16u;
    std::vector<fuse::u32> parallelGrid(outer * inner, 0u);
    std::vector<fuse::u32> serialGrid(outer * inner, 0u);

    for (fuse::u32 o = 0; o < outer; ++o) {
        for (fuse::u32 i = 0; i < inner; ++i) {
            serialGrid[o * inner + i] = o * inner + i;
        }
    }

    // One outer task iterates rows serially so only one worker enters cooperative wait.
    withScheduler(4, [&] {
        fuse::jobs::parallel_for(0u, outer, outer, [&](fuse::u32 o) {
            fuse::jobs::parallel_for(0u, inner, 3u, [&](fuse::u32 i) {
                parallelGrid[o * inner + i] = o * inner + i;
            });
        });
    });

    for (fuse::u32 o = 0; o < outer; ++o) {
        for (fuse::u32 i = 0; i < inner; ++i) {
            const fuse::u32 idx = o * inner + i;
            if (parallelGrid[idx] != serialGrid[idx]) {
                std::fprintf(stderr, "FAIL: nested parallel_for mismatch at (%u,%u)\n", o, i);
                ++g_failures;
                return;
            }
        }
    }
}

void testNestedParallelForSerialFallbackParity() {
    constexpr fuse::u32 outer = 12u;
    constexpr fuse::u32 inner = 10u;
    std::vector<fuse::u32> parallelGrid(outer * inner, 0u);
    std::vector<fuse::u32> fallbackGrid(outer * inner, 0u);

    auto fillGrid = [](std::vector<fuse::u32>& grid, fuse::u32 outerCount, fuse::u32 innerCount) {
        fuse::jobs::parallel_for(0u, outerCount, outerCount, [&](fuse::u32 o) {
            fuse::jobs::parallel_for(0u, innerCount, 2u, [&](fuse::u32 i) {
                grid[o * innerCount + i] = (o + 1u) * (i + 1u);
            });
        });
    };

    withScheduler(4, [&] { fillGrid(parallelGrid, outer, inner); });
    withScheduler(0, [&] { fillGrid(fallbackGrid, outer, inner); });

    for (fuse::u32 idx = 0; idx < outer * inner; ++idx) {
        if (parallelGrid[idx] != fallbackGrid[idx]) {
            std::fprintf(stderr, "FAIL: nested parallel vs fallback mismatch at index %u\n", idx);
            ++g_failures;
            return;
        }
    }
}

void testNestedParallelForWithCooperativeWait() {
    if (!fuse::platform::cooperativeFibersAvailable()) {
        std::printf("SKIP: cooperative fibers unavailable for nested wait regression\n");
        return;
    }

    // One outer chunk so a single worker enters cooperative wait while peers drain inner jobs.
    constexpr fuse::u32 outer = 32u;
    constexpr fuse::u32 inner = 64u;
    std::atomic<fuse::u32> visitCount{0};

    withScheduler(4, [&] {
        fuse::jobs::parallel_for(0u, outer, outer, [&](fuse::u32 /*o*/) {
            fuse::jobs::parallel_for(0u, inner, 4u, [&](fuse::u32 /*i*/) {
                visitCount.fetch_add(1u, std::memory_order_relaxed);
            });
        });
    });

    expectEq(visitCount.load(std::memory_order_relaxed), outer * inner,
             "nested parallel_for with cooperative waits visits every index");
}

} // namespace

int main() {
    testCounterWait();
    testCooperativeWorkerWait();
    testParallelForMatchesSerial();
    testSingleThreadFallback();
    testParallelSerialFallbackParity();
    testNestedParallelForParity();
    testNestedParallelForSerialFallbackParity();
    testNestedParallelForWithCooperativeWait();

    if (g_failures == 0) {
        std::printf("fuse_core job tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core job tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
