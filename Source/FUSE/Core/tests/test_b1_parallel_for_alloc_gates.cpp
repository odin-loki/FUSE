// B1 parallel_for dispatch gates:
//   - Steady-state JobScheduler::parallel_for makes zero heap allocations on any thread
//     (counted through this binary's replaced global operator new) for 1/2/4 workers, including
//     nested parallel_for from inside chunks and parallel_for issued from inside a job, with
//     simulated preemption of chunk-running threads (peak fiber park depth, as on a loaded host).
//   - Bursts of 250 directly submitted jobs per frame (with work stealing) make zero steady-state
//     heap allocations: stealing never grows the thief's ring.
//   - Concurrent parallel_for from several non-worker threads stays correct.
//   - Dispatch latency percentiles for a 4096-item / grain-256 parallel_for at 0/1/2/4 workers.
//     Release builds (no sanitizer) enforce a loose median bound at 1 and 4 workers.
//   - Idle workers go back to sleep instead of spinning (process CPU time over an idle window).
// Every multi-worker section runs under a watchdog so a deadlock fails instead of hanging CI.

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

// Over-aligned replacement new: the Windows CRT has no std::aligned_alloc, and its _aligned_malloc
// blocks must be released with _aligned_free (never free()), so the align_val_t deletes differ.
#if defined(_WIN32)
static inline void* fuseTestAlignedAlloc(std::size_t alignment, std::size_t size) {
    return _aligned_malloc(size, alignment);
}
static inline void fuseTestAlignedFree(void* ptr) {
    _aligned_free(ptr);
}
#else
static inline void* fuseTestAlignedAlloc(std::size_t alignment, std::size_t size) {
    return std::aligned_alloc(alignment, size);
}
static inline void fuseTestAlignedFree(void* ptr) {
    std::free(ptr);
}
#endif

// ---- global heap counter (whole binary, every thread) ------------------------------------------

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};
} // namespace

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1u) / align * align;
    if (void* p = fuseTestAlignedAlloc(align, rounded)) {
        return p;
    }
    throw std::bad_alloc();
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::align_val_t) noexcept { fuseTestAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::align_val_t) noexcept { fuseTestAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { fuseTestAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { fuseTestAlignedFree(ptr); }

namespace {

using fuse::u32;
using fuse::u64;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

#if defined(NDEBUG)
constexpr bool kOptimizedBuild = !kSanitized;
#else
constexpr bool kOptimizedBuild = false;
#endif

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// Aborts the process with a message when a section runs longer than `seconds` (deadlock guard).
class Watchdog {
public:
    Watchdog(const char* label, int seconds) : m_label(label) {
        m_thread = std::thread([this, seconds] {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_cv.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_done; })) {
                std::fprintf(stderr, "FAIL: watchdog expired in '%s' (deadlock)\n", m_label);
                std::fflush(stderr);
                std::_Exit(EXIT_FAILURE);
            }
        });
    }

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

private:
    const char* m_label;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done = false;
    std::thread m_thread;
};

constexpr u32 kItems = 4096;
constexpr u32 kGrain = 256;

/// Plain 4096-item parallel_for with a cheap body; returns true when every item ran exactly once.
bool runFlat(std::vector<u32>& data, u32 stamp) {
    fuse::jobs::parallel_for(0u, kItems, kGrain, [&](u32 i) { data[i] = stamp + i; });
    for (u32 i = 0; i < kItems; ++i) {
        if (data[i] != stamp + i) {
            return false;
        }
    }
    return true;
}

/// Outer parallel_for whose chunks each run an inner parallel_for (fiber-park wait path on workers).
/// With `stallChunks`, a few inner items sleep briefly, standing in for the thread running them being
/// preempted on a loaded host: waiters then park and workers start further blocking jobs on other
/// fibers, which is how the park depth (and so the fiber pool) peaks under load.
bool runNested(std::vector<u32>& data, u32 stamp, bool stallChunks = false) {
    constexpr u32 kOuter = 8;
    constexpr u32 kInner = kItems / kOuter;
    fuse::jobs::parallel_for(0u, kOuter, 1u, [&](u32 outer) {
        fuse::jobs::parallel_for(0u, kInner, 64u, [&](u32 inner) {
            const u32 i = outer * kInner + inner;
            data[i] = stamp ^ i;
            if (stallChunks && ((stamp * 2654435761u) ^ i) % 997u == 0u) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
    });
    for (u32 i = 0; i < kItems; ++i) {
        if (data[i] != (stamp ^ i)) {
            return false;
        }
    }
    return true;
}

/// parallel_for issued from inside a submitted job, waited on through a JobCounter.
bool runFromJob(std::vector<u32>& data, u32 stamp, fuse::jobs::JobCounter& counter) {
    struct Context {
        std::vector<u32>* data;
        fuse::jobs::JobCounter* counter;
        u32 stamp;
    } context{&data, &counter, stamp};
    counter.reset(1);
    // One captured pointer keeps the job itself inside std::function's inline storage.
    fuse::jobs::JobScheduler::instance().submit([ctx = &context]() {
        fuse::jobs::parallel_for(0u, kItems, kGrain, [ctx](u32 i) { (*ctx->data)[i] = ctx->stamp * 3u + i; });
        ctx->counter->signal();
    });
    counter.wait();
    for (u32 i = 0; i < kItems; ++i) {
        if (data[i] != stamp * 3u + i) {
            return false;
        }
    }
    return true;
}

void testZeroAllocations(u32 workers) {
    char label[96];
    std::snprintf(label, sizeof(label), "parallel_for zero-alloc (%u workers)", workers);
    Watchdog watchdog(label, 300);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);

    std::vector<u32> data(kItems, 0u);
    fuse::jobs::JobCounter counter{0};
    bool ok = true;

    // Warm-up grows queues, fiber pools and dispatch pools to their steady-state size.
    for (u32 iter = 0; iter < 300; ++iter) {
        ok = runFlat(data, iter) && ok;
        ok = runNested(data, iter) && ok;
        ok = runFromJob(data, iter, counter) && ok;
    }

    constexpr u32 kCalls = 1000;
    const u64 flatBefore = g_heapAllocations.load(std::memory_order_acquire);
    for (u32 iter = 0; iter < kCalls; ++iter) {
        ok = runFlat(data, iter + 7u) && ok;
    }
    const u64 flatAllocs = g_heapAllocations.load(std::memory_order_acquire) - flatBefore;

    const u64 mixedBefore = g_heapAllocations.load(std::memory_order_acquire);
    for (u32 iter = 0; iter < kCalls; ++iter) {
        ok = runNested(data, iter + 11u, true) && ok;
        ok = runFromJob(data, iter + 13u, counter) && ok;
    }
    const u64 mixedAllocs = g_heapAllocations.load(std::memory_order_acquire) - mixedBefore;

    std::printf("  workers=%u: heap allocations over %u parallel_for calls: flat=%llu nested+from-job=%llu\n",
                workers, kCalls, static_cast<unsigned long long>(flatAllocs),
                static_cast<unsigned long long>(mixedAllocs));

    std::snprintf(label, sizeof(label), "parallel_for results correct (%u workers)", workers);
    expectTrue(ok, label);
    std::snprintf(label, sizeof(label), "1000 steady-state parallel_for make 0 heap allocations (%u workers)",
                  workers);
    expectTrue(flatAllocs == 0u, label);
    // Nested waits park job fibers; a new high-water mark of parked fibers creates a fiber (pool
    // growth, not per-call cost). Sanitizer timing can reach one after warm-up, so allow a few there.
    const u64 mixedBudget = kSanitized ? 16u : 0u;
    std::snprintf(label, sizeof(label), "nested / in-job parallel_for make <= %llu heap allocations (%u workers)",
                  static_cast<unsigned long long>(mixedBudget), workers);
    expectTrue(mixedAllocs <= mixedBudget, label);

    scheduler.shutdown();
}

/// Bursts of directly submitted small jobs (a frame's worth of fire-and-forget work). Round-robin
/// pushes fill every worker's ring and idle workers steal half of a victim's queue into their own
/// ring; steady state must not grow any ring, whichever worker ends up holding the jobs.
void testSubmitBurstsDoNotGrowQueues(u32 workers) {
    char label[112];
    std::snprintf(label, sizeof(label), "submit bursts zero-alloc (%u workers)", workers);
    Watchdog watchdog(label, 300);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);

    constexpr u32 kJobsPerFrame = 250;
    std::atomic<u32> ran{0};
    fuse::jobs::JobCounter counter{0};
    struct Context {
        std::atomic<u32>* ran;
        fuse::jobs::JobCounter* counter;
    } context{&ran, &counter};

    const auto runFrame = [&]() {
        counter.reset(kJobsPerFrame);
        for (u32 j = 0; j < kJobsPerFrame; ++j) {
            // Uneven job cost keeps some workers busy while others go idle and steal.
            scheduler.submit([ctx = &context]() {
                const u32 n = ctx->ran->fetch_add(1u, std::memory_order_relaxed);
                if ((n & 31u) == 0u) {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                }
                ctx->counter->signal();
            });
        }
        counter.wait();
    };

    for (u32 frame = 0; frame < 200; ++frame) {
        runFrame();
    }
    constexpr u32 kFrames = 1000;
    const u64 before = g_heapAllocations.load(std::memory_order_acquire);
    for (u32 frame = 0; frame < kFrames; ++frame) {
        runFrame();
    }
    const u64 allocs = g_heapAllocations.load(std::memory_order_acquire) - before;
    std::printf("  workers=%u: heap allocations over %u frames of %u submitted jobs: %llu\n", workers, kFrames,
                kJobsPerFrame, static_cast<unsigned long long>(allocs));
    std::snprintf(label, sizeof(label), "all submitted jobs ran (%u workers)", workers);
    expectTrue(ran.load() == (200u + kFrames) * kJobsPerFrame, label);
    std::snprintf(label, sizeof(label), "steady-state submit bursts + stealing make 0 heap allocations (%u workers)",
                  workers);
    expectTrue(allocs == 0u, label);
    scheduler.shutdown();
}

void testConcurrentCallers() {
    Watchdog watchdog("parallel_for concurrent callers", 300);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);

    constexpr u32 kThreads = 4;
    constexpr u32 kIterations = 500;
    std::atomic<u32> bad{0};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bad, t] {
            std::vector<u32> data(kItems, 0u);
            for (u32 iter = 0; iter < kIterations; ++iter) {
                const u32 stamp = t * 100000u + iter;
                if (!runFlat(data, stamp) || !runNested(data, stamp)) {
                    bad.fetch_add(1u, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    expectTrue(bad.load() == 0u, "concurrent parallel_for from 4 external threads stays correct");
    scheduler.shutdown();
}

struct LatencyStats {
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

LatencyStats measureLatency(u32 workers) {
    char label[64];
    std::snprintf(label, sizeof(label), "parallel_for latency (%u workers)", workers);
    Watchdog watchdog(label, 300);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);

    std::vector<u32> data(kItems, 0u);
    for (u32 iter = 0; iter < 200; ++iter) {
        runFlat(data, iter);
    }

    constexpr u32 kSamples = 3000;
    std::vector<double> samples(kSamples);
    for (u32 s = 0; s < kSamples; ++s) {
        const auto start = std::chrono::steady_clock::now();
        fuse::jobs::parallel_for(0u, kItems, kGrain, [&](u32 i) { data[i] = s + i; });
        const auto stop = std::chrono::steady_clock::now();
        samples[s] = std::chrono::duration<double, std::micro>(stop - start).count();
    }
    std::sort(samples.begin(), samples.end());
    LatencyStats stats;
    stats.p50 = samples[kSamples / 2];
    stats.p90 = samples[kSamples * 9 / 10];
    stats.p99 = samples[kSamples * 99 / 100];
    stats.max = samples.back();
    std::printf("  workers=%u: dispatch latency %u items / grain %u: p50=%.2f us p90=%.2f us p99=%.2f us max=%.2f us\n",
                workers, kItems, kGrain, stats.p50, stats.p90, stats.p99, stats.max);

    scheduler.shutdown();
    return stats;
}

#if defined(__linux__) || defined(__APPLE__)
/// Idle workers must stop spinning shortly after the last job and sleep.
void testIdleWorkersSleep() {
    Watchdog watchdog("idle workers sleep", 120);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);

    std::vector<u32> data(kItems, 0u);
    for (u32 iter = 0; iter < 200; ++iter) {
        runFlat(data, iter);
    }
    // Let the post-work spin window lapse, then sample process CPU time over an idle window.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::clock_t cpuStart = std::clock();
    const auto wallStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double wallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count();
    const double cpuMs = 1000.0 * static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
    std::printf("  idle: 4 workers used %.2f ms CPU over %.1f ms wall\n", cpuMs, wallMs);
    // Four spinning workers would burn ~4x wall time; a sleeping pool only pays its safety-net
    // timeouts (about 1% on a loaded host). Allow generous noise.
    expectTrue(cpuMs < 0.10 * wallMs, "idle workers sleep (process CPU < 10% of one core while idle)");
    scheduler.shutdown();
}
#endif

} // namespace

int main() {
    std::printf("B1 parallel_for allocation / latency gates\n");

    for (u32 workers : {1u, 2u, 4u}) {
        testZeroAllocations(workers);
    }
    for (u32 workers : {2u, 4u}) {
        testSubmitBurstsDoNotGrowQueues(workers);
    }
    testConcurrentCallers();

    LatencyStats byWorkers[5];
    for (u32 workers : {0u, 1u, 2u, 4u}) {
        byWorkers[workers] = measureLatency(workers);
    }
    if (kOptimizedBuild) {
        expectTrue(byWorkers[4].p50 < 50.0, "Release: parallel_for(4096, grain 256) median < 50 us at 4 workers");
        expectTrue(byWorkers[1].p50 < 50.0, "Release: parallel_for(4096, grain 256) median < 50 us at 1 worker");
    } else {
        std::printf("  (latency bounds enforced only in optimized, unsanitized builds)\n");
    }

#if defined(__linux__) || defined(__APPLE__)
    testIdleWorkersSleep();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "%d parallel_for gate check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("All parallel_for gates passed\n");
    return EXIT_SUCCESS;
}
