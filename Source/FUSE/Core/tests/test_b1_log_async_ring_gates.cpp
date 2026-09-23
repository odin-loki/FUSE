// B1.6 follow-up gate "logging.async_ring": lock-free async logger ring (fuse::log::Logger::startAsync).
//   - Multi-producer stress (4 threads x 100k messages): no message lost while the ring is not allowed
//     to overflow, and every producer's messages reach the sink in the order it emitted them.
//   - Forced overflow: with the consumer held inside the sink, producers never block, the ring
//     accepts exactly capacity-1 messages and droppedCount() rises by exactly the rest.
//   - Zero heap allocations on the producer path (this binary's replaced global operator new counts
//     every allocation made by a producer thread while it logs), and none anywhere during steady
//     state (consumer + sink included).
//   - stopAsync() while producers keep logging loses nothing and keeps per-thread order across the
//     switch back to synchronous delivery; Fatal flushes the ring first.
//   - Producer latency percentiles (per log() call) are printed; loose median bound only in
//     optimized non-sanitizer builds.
// Every section runs under a watchdog so a deadlock fails instead of hanging CI.

#include <fuse/log/logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>


// ---- heap counter ------------------------------------------------------------------------------

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};
std::atomic<std::uint64_t> g_producerAllocations{0};
thread_local bool t_countProducerAllocs = false;

void noteAllocation() {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    if (t_countProducerAllocs) {
        g_producerAllocations.fetch_add(1u, std::memory_order_relaxed);
    }
}
} // namespace

// Replacement allocation/deallocation functions stay out of line: once GCC inlines one of them into
// a std::allocator call site it pairs its malloc()/free() with the other side's builtin
// ::operator new/delete and reports a false -Wmismatched-new-delete (replacement functions must not
// be inline anyway, [replacement.functions]).
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    noteAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    noteAllocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1u) / align * align;
    if (void* p = std::aligned_alloc(align, rounded)) {
        return p;
    }
    throw std::bad_alloc();
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}

FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { std::free(ptr); }

namespace {

using fuse::u32;
using fuse::u64;
using fuse::log::Level;
using fuse::log::Logger;

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

// Shipping (FUSE_NO_LOGGING) drops every sub-Fatal message before it reaches the async ring: the
// sections below then prove the stripped behaviour instead (nothing enqueued, dropped or delivered,
// producers still allocation-free and non-blocking; Fatal still delivered synchronously).
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
constexpr bool kLoggingStripped = true;
#else
constexpr bool kLoggingStripped = false;
#endif

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

u64 nowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// ---- watchdog ----------------------------------------------------------------------------------

struct Watchdog {
    std::atomic<bool> done{false};
    std::thread thread;
    Watchdog(const char* section, u32 seconds) {
        thread = std::thread([this, section, seconds] {
            const u64 deadline = nowNs() + static_cast<u64>(seconds) * 1000000000ull;
            while (!done.load(std::memory_order_acquire)) {
                if (nowNs() > deadline) {
                    std::fprintf(stderr, "FAIL: watchdog: '%s' exceeded %u s (deadlock?)\n", section, seconds);
                    std::fflush(stderr);
                    std::_Exit(EXIT_FAILURE);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~Watchdog() {
        done.store(true, std::memory_order_release);
        thread.join();
    }
};

// ---- capture sink (called by one thread at a time; allocation-free) ----------------------------

constexpr u32 kMaxWriters = 8u;

struct Capture {
    u64 count[kMaxWriters]{};
    long long lastSeq[kMaxWriters]{};
    u64 orderViolations = 0;
    u64 gaps = 0; // seq != lastSeq + 1 (only meaningful when nothing may be dropped)
    u64 malformed = 0;
    u64 other = 0;
    std::atomic<bool> gateEntered{false};
    std::atomic<bool> gateRelease{false};

    void reset() {
        for (u32 i = 0; i < kMaxWriters; ++i) {
            count[i] = 0;
            lastSeq[i] = -1;
        }
        orderViolations = gaps = malformed = other = 0;
        gateEntered.store(false);
        gateRelease.store(false);
    }
    u64 total() const {
        u64 t = 0;
        for (u32 i = 0; i < kMaxWriters; ++i) {
            t += count[i];
        }
        return t;
    }
};

bool parseU32(const char*& p, u32& out) {
    if (*p < '0' || *p > '9') {
        return false;
    }
    out = 0;
    while (*p >= '0' && *p <= '9') {
        out = out * 10u + static_cast<u32>(*p - '0');
        ++p;
    }
    return true;
}

void captureSink(Level /*level*/, const char* message, void* userData) {
    auto* cap = static_cast<Capture*>(userData);
    if (std::strncmp(message, "gate", 4) == 0) {
        cap->gateEntered.store(true, std::memory_order_release);
        while (!cap->gateRelease.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return;
    }
    if (message[0] != 'w' || message[1] != '=') {
        ++cap->other;
        return;
    }
    const char* p = message + 2;
    u32 writer = 0;
    u32 seq = 0;
    if (!parseU32(p, writer) || std::strncmp(p, " s=", 3) != 0) {
        ++cap->malformed;
        return;
    }
    p += 3;
    if (!parseU32(p, seq) || std::strcmp(p, " end") != 0 || writer >= kMaxWriters) {
        ++cap->malformed;
        return;
    }
    const long long s = static_cast<long long>(seq);
    if (s <= cap->lastSeq[writer]) {
        ++cap->orderViolations;
    }
    if (s != cap->lastSeq[writer] + 1) {
        ++cap->gaps;
    }
    cap->lastSeq[writer] = s;
    ++cap->count[writer];
}

// ---- producers ---------------------------------------------------------------------------------

struct ProducerPlan {
    u32 threads = 4u;
    u32 messagesPerThread = 100000u;
    /// flush() after this many messages per thread (0 = never); bounds ring occupancy.
    u32 flushEvery = 0u;
    bool recordLatency = true;
};

struct ProducerResult {
    std::vector<std::vector<u32>> latencyNs;
    u64 producerAllocs = 0;
    u64 wallNs = 0;
};

/// Runs the producers with a start barrier. `midRun` (optional) runs on the calling thread once all
/// producers have started.
template <typename MidRun>
ProducerResult runProducers(const ProducerPlan& plan, MidRun&& midRun) {
    ProducerResult result;
    result.latencyNs.assign(plan.threads, std::vector<u32>(plan.recordLatency ? plan.messagesPerThread : 0u));
    std::atomic<u32> ready{0};
    std::atomic<bool> go{false};
    std::atomic<u32> finished{0};
    const u64 allocsBefore = g_producerAllocations.load();
    std::vector<std::thread> threads;
    threads.reserve(plan.threads);
    for (u32 t = 0; t < plan.threads; ++t) {
        threads.emplace_back([&, t] {
            u32* lat = plan.recordLatency ? result.latencyNs[t].data() : nullptr;
            Logger& logger = Logger::instance();
            ready.fetch_add(1u);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            t_countProducerAllocs = true;
            for (u32 seq = 0; seq < plan.messagesPerThread; ++seq) {
                const u64 t0 = lat ? nowNs() : 0u;
                logger.log(Level::Info, fuse::log::Channel::Core, "w=%u s=%u end", t, seq);
                if (lat) {
                    lat[seq] = static_cast<u32>(std::min<u64>(nowNs() - t0, 0xFFFFFFFFull));
                }
                if (plan.flushEvery != 0u && (seq + 1u) % plan.flushEvery == 0u) {
                    t_countProducerAllocs = false;
                    logger.flush();
                    t_countProducerAllocs = true;
                }
            }
            t_countProducerAllocs = false;
            finished.fetch_add(1u);
        });
    }
    while (ready.load() != plan.threads) {
        std::this_thread::yield();
    }
    const u64 start = nowNs();
    go.store(true, std::memory_order_release);
    midRun();
    while (finished.load() != plan.threads) {
        std::this_thread::yield();
    }
    result.wallNs = nowNs() - start;
    for (std::thread& th : threads) {
        th.join();
    }
    result.producerAllocs = g_producerAllocations.load() - allocsBefore;
    return result;
}

ProducerResult runProducers(const ProducerPlan& plan) {
    return runProducers(plan, [] {});
}

struct Percentiles {
    u32 p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Percentiles percentiles(const std::vector<std::vector<u32>>& perThread) {
    std::vector<u32> all;
    for (const auto& v : perThread) {
        all.insert(all.end(), v.begin(), v.end());
    }
    Percentiles p;
    if (all.empty()) {
        return p;
    }
    std::sort(all.begin(), all.end());
    auto at = [&](double q) { return all[static_cast<std::size_t>(q * static_cast<double>(all.size() - 1u))]; };
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = all.back();
    return p;
}

void printLatency(const char* label, const Percentiles& p) {
    std::printf("  %-34s p50=%u ns p90=%u ns p99=%u ns p99.9=%u ns max=%u ns\n", label, p.p50, p.p90, p.p99,
                p.p999, p.max);
}

// ---- sections ----------------------------------------------------------------------------------

Capture g_capture;

void resetLogger() {
    Logger& logger = Logger::instance();
    logger.stopAsync();
    logger.setMinLevel(Level::Trace);
    logger.setEnabledChannels(static_cast<u32>(fuse::log::Channel::All));
    g_capture.reset();
    logger.setSink(captureSink, &g_capture);
}

void testStressNoLossPerThreadOrder() {
    Watchdog dog("stress no-loss", 300u);
    resetLogger();
    Logger& logger = Logger::instance();
    constexpr u32 kCapacity = 1u << 16;
    expectTrue(logger.startAsync({kCapacity}), "startAsync succeeds");
    expectTrue(logger.isAsync(), "isAsync after startAsync");
    expectTrue(!logger.startAsync({kCapacity}), "second startAsync is refused");

    ProducerPlan plan;
    plan.threads = 4u;
    plan.messagesPerThread = 100000u;
    // Each thread has at most 8192 unflushed messages, so occupancy <= 4 * 8192 < capacity: no drops
    // are possible and any missing message is a loss.
    plan.flushEvery = 8192u;
    const u64 droppedBefore = logger.droppedCount();
    const fuse::log::AsyncStats statsBefore = logger.asyncStats();
    ProducerResult r = runProducers(plan);
    logger.flush();
    const fuse::log::AsyncStats stats = logger.asyncStats();

    const u64 expected = static_cast<u64>(plan.threads) * plan.messagesPerThread;
    bool perThreadComplete = true;
    for (u32 t = 0; t < plan.threads; ++t) {
        perThreadComplete = perThreadComplete && g_capture.count[t] == plan.messagesPerThread &&
                            g_capture.lastSeq[t] == static_cast<long long>(plan.messagesPerThread) - 1;
    }
    const Percentiles lat = percentiles(r.latencyNs);
    std::printf("  stress: %u threads x %u msgs, capacity %u: delivered=%llu dropped=%llu order_violations=%llu "
                "gaps=%llu malformed=%llu producer_allocs=%llu wall=%.1f ms (%.2f M msg/s)\n",
                plan.threads, plan.messagesPerThread, stats.capacity,
                static_cast<unsigned long long>(g_capture.total()),
                static_cast<unsigned long long>(logger.droppedCount() - droppedBefore),
                static_cast<unsigned long long>(g_capture.orderViolations),
                static_cast<unsigned long long>(g_capture.gaps), static_cast<unsigned long long>(g_capture.malformed),
                static_cast<unsigned long long>(r.producerAllocs), static_cast<double>(r.wallNs) / 1e6,
                static_cast<double>(expected) * 1e3 / static_cast<double>(r.wallNs));
    printLatency("producer log() latency (async):", lat);

    expectTrue(logger.droppedCount() == droppedBefore, "stress: nothing dropped while occupancy < capacity");
    expectTrue(g_capture.orderViolations == 0u && g_capture.gaps == 0u, "stress: per-producer order preserved");
    expectTrue(g_capture.malformed == 0u, "stress: no torn/malformed messages");
    expectTrue(r.producerAllocs == 0u, "stress: zero heap allocations on the producer path");
    if constexpr (kLoggingStripped) {
        expectTrue(g_capture.total() == 0u && g_capture.other == 0u, "shipping stress: no Info message reaches the sink");
        expectTrue(stats.enqueued == statsBefore.enqueued, "shipping stress: nothing enqueued on the async ring");
        expectTrue(stats.delivered == statsBefore.delivered, "shipping stress: nothing delivered by the consumer");
    } else {
        expectTrue(g_capture.total() == expected, "stress: every message delivered (no loss)");
        expectTrue(perThreadComplete, "stress: each producer's full sequence delivered");
        expectTrue(stats.delivered >= expected, "stress: asyncStats counts deliveries");
    }

    // Record ring still works in async mode (snapshotRecords flushes first).
    logger.log(Level::Warn, fuse::log::Channel::Renderer, "snapshot probe %d", 42);
    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    bool found = false;
    for (u32 i = 0; i < snap.count; ++i) {
        found = found || (snap.records[i].level == Level::Warn &&
                          snap.records[i].channel == fuse::log::Channel::Renderer &&
                          std::strcmp(snap.records[i].message, "snapshot probe 42") == 0);
    }
    if constexpr (kLoggingStripped) {
        expectTrue(!found, "shipping async: Warn entry is never recorded");
    } else {
        expectTrue(found, "async: snapshotRecords sees an entry logged just before it");
    }
    logger.stopAsync();
    expectTrue(!logger.isAsync(), "stopAsync returns to synchronous mode");
}

void testSteadyStateHeapWholeProcess() {
    // Same traffic, but every allocation in the process is counted (consumer thread and sink too).
    Watchdog dog("steady heap", 300u);
    resetLogger();
    Logger& logger = Logger::instance();
    expectTrue(logger.startAsync({1u << 12}), "startAsync (heap section)");
    constexpr u32 kThreads = 4u;
    constexpr u32 kPer = 20000u;
    std::atomic<bool> go{false};
    std::atomic<u32> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1u);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (u32 s = 0; s < kPer; ++s) {
                logger.log(Level::Info, fuse::log::Channel::Core, "w=%u s=%u end", t, s);
                if ((s + 1u) % 512u == 0u) {
                    logger.flush();
                }
            }
        });
    }
    while (ready.load() != kThreads) {
        std::this_thread::yield();
    }
    const u64 before = g_heapAllocations.load();
    go.store(true, std::memory_order_release);
    for (std::thread& th : threads) {
        th.join();
    }
    logger.flush();
    const u64 during = g_heapAllocations.load() - before;
    std::printf("  steady state: %u msgs, whole-process heap allocations during logging=%llu\n", kThreads * kPer,
                static_cast<unsigned long long>(during));
    expectTrue(during == 0u, "steady state: zero heap allocations anywhere (producers, consumer, flush)");
    if constexpr (kLoggingStripped) {
        expectTrue(g_capture.total() == 0u, "shipping steady state: no Info message reaches the sink");
    } else {
        expectTrue(g_capture.total() == static_cast<u64>(kThreads) * kPer && g_capture.gaps == 0u,
                   "steady state: no loss, per-producer order");
    }
    logger.stopAsync();
}

void testForcedOverflowExactDropCount() {
    Watchdog dog("forced overflow", 120u);
    resetLogger();
    Logger& logger = Logger::instance();
    constexpr u32 kCapacity = 256u;
    expectTrue(logger.startAsync({kCapacity}), "startAsync (overflow section)");

    if constexpr (kLoggingStripped) {
        // The gate message would never reach the sink: instead prove that a tiny ring cannot
        // overflow, because stripped producers never enqueue (no drops, never block, no allocations).
        ProducerPlan stripped;
        stripped.threads = 4u;
        stripped.messagesPerThread = 10000u;
        const u64 droppedBefore = logger.droppedCount();
        const fuse::log::AsyncStats statsBefore = logger.asyncStats();
        const ProducerResult r = runProducers(stripped);
        logger.flush();
        const fuse::log::AsyncStats statsAfter = logger.asyncStats();
        std::printf("  overflow (shipping strip): capacity %u, produced=%llu enqueued=%llu dropped=%llu "
                    "delivered=%llu producer_allocs=%llu\n",
                    kCapacity,
                    static_cast<unsigned long long>(static_cast<u64>(stripped.threads) * stripped.messagesPerThread),
                    static_cast<unsigned long long>(statsAfter.enqueued - statsBefore.enqueued),
                    static_cast<unsigned long long>(logger.droppedCount() - droppedBefore),
                    static_cast<unsigned long long>(g_capture.total()),
                    static_cast<unsigned long long>(r.producerAllocs));
        expectTrue(logger.droppedCount() == droppedBefore, "shipping overflow: nothing dropped");
        expectTrue(statsAfter.enqueued == statsBefore.enqueued, "shipping overflow: nothing enqueued");
        expectTrue(g_capture.total() == 0u && !g_capture.gateEntered.load(), "shipping overflow: sink never called");
        expectTrue(r.producerAllocs == 0u, "shipping overflow: zero heap allocations on the producer path");
        logger.stopAsync();
        return;
    }

    // Park the consumer inside the sink while holding the first slot.
    logger.log(Level::Info, fuse::log::Channel::Core, "gate");
    while (!g_capture.gateEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ProducerPlan plan;
    plan.threads = 4u;
    plan.messagesPerThread = 10000u;
    const u64 droppedBefore = logger.droppedCount();
    const fuse::log::AsyncStats statsBefore = logger.asyncStats();
    // Producers must all finish while the consumer is still blocked: that is the "never block" proof.
    ProducerResult r = runProducers(plan);
    const u64 droppedWhileBlocked = logger.droppedCount() - droppedBefore;
    const fuse::log::AsyncStats statsBlocked = logger.asyncStats();
    g_capture.gateRelease.store(true, std::memory_order_release);
    logger.flush();

    const u64 produced = static_cast<u64>(plan.threads) * plan.messagesPerThread;
    const u64 accepted = kCapacity - 1u; // the gate message still occupies one slot
    const Percentiles lat = percentiles(r.latencyNs);
    std::printf("  overflow: capacity %u, produced=%llu accepted=%llu delivered=%llu dropped=%llu "
                "(expected %llu) order_violations=%llu producer_allocs=%llu\n",
                kCapacity, static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(statsBlocked.enqueued - statsBefore.enqueued),
                static_cast<unsigned long long>(g_capture.total()),
                static_cast<unsigned long long>(droppedWhileBlocked),
                static_cast<unsigned long long>(produced - accepted),
                static_cast<unsigned long long>(g_capture.orderViolations),
                static_cast<unsigned long long>(r.producerAllocs));
    printLatency("producer log() latency (ring full):", lat);

    expectTrue(droppedWhileBlocked == produced - accepted, "overflow: drop count is exact");
    expectTrue(statsBlocked.enqueued - statsBefore.enqueued == accepted, "overflow: ring accepted capacity-1");
    expectTrue(g_capture.total() == accepted, "overflow: every accepted message delivered");
    expectTrue(g_capture.total() + droppedWhileBlocked == produced, "overflow: delivered + dropped == produced");
    expectTrue(g_capture.orderViolations == 0u, "overflow: surviving messages keep per-producer order");
    expectTrue(r.producerAllocs == 0u, "overflow: zero heap allocations on the drop path");

    // After the backlog drains the ring accepts again.
    g_capture.reset();
    logger.log(Level::Info, fuse::log::Channel::Core, "w=0 s=0 end");
    logger.flush();
    expectTrue(g_capture.count[0] == 1u, "overflow: ring accepts again after draining");
    logger.stopAsync();
}

void testStopWhileLoggingKeepsOrder() {
    Watchdog dog("stop while logging", 300u);
    resetLogger();
    Logger& logger = Logger::instance();
    expectTrue(logger.startAsync({1u << 15}), "startAsync (stop section)");
    ProducerPlan plan;
    plan.threads = 4u;
    plan.messagesPerThread = 30000u;
    plan.flushEvery = 4096u; // 4 * 4096 < capacity: no drops
    plan.recordLatency = false;
    runProducers(plan, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        logger.stopAsync(); // producers keep logging, now synchronously
    });
    logger.flush();
    const u64 expected = static_cast<u64>(plan.threads) * plan.messagesPerThread;
    std::printf("  stop-while-logging: delivered=%llu/%llu gaps=%llu order_violations=%llu\n",
                static_cast<unsigned long long>(g_capture.total()), static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(g_capture.gaps),
                static_cast<unsigned long long>(g_capture.orderViolations));
    expectTrue(!logger.isAsync(), "stop: logger synchronous after stopAsync");
    if constexpr (kLoggingStripped) {
        expectTrue(g_capture.total() == 0u, "shipping stop: no Info message reaches the sink in either mode");
    } else {
        expectTrue(g_capture.total() == expected, "stop: no loss across async -> sync switch");
    }
    expectTrue(g_capture.gaps == 0u && g_capture.orderViolations == 0u,
               "stop: per-producer order preserved across the switch");
}

struct FatalCapture {
    u32 lines = 0;
    bool fatalAfterInfo = false;
    bool sawInfo = false;
};

void fatalSink(Level level, const char* message, void* userData) {
    auto* cap = static_cast<FatalCapture*>(userData);
    ++cap->lines;
    if (std::strcmp(message, "before fatal") == 0) {
        cap->sawInfo = true;
    }
    if (level == Level::Fatal) {
        cap->fatalAfterInfo = cap->sawInfo;
    }
}

void testFatalFlushesAndIsSynchronous() {
    Watchdog dog("fatal", 60u);
    resetLogger();
    Logger& logger = Logger::instance();
    FatalCapture cap;
    logger.setSink(fatalSink, &cap);
    expectTrue(logger.startAsync({64u}), "startAsync (fatal section)");
    logger.log(Level::Info, fuse::log::Channel::Core, "before fatal");
    logger.log(Level::Fatal, fuse::log::Channel::Core, "fatal line");
    // No flush: Fatal is delivered before log() returns, after everything queued ahead of it.
    if constexpr (kLoggingStripped) {
        // Fatal is never stripped: still synchronous; the Info line before it was dropped.
        expectTrue(cap.lines == 1u && !cap.sawInfo, "shipping fatal: only the Fatal line is delivered, synchronously");
    } else {
        expectTrue(cap.lines == 2u && cap.fatalAfterInfo, "fatal: synchronous and ordered after queued messages");
    }
    logger.stopAsync();
}

void testSynchronousLatencyBaseline() {
    Watchdog dog("sync baseline", 300u);
    resetLogger();
    ProducerPlan plan;
    plan.threads = 4u;
    plan.messagesPerThread = 25000u;
    ProducerResult r = runProducers(plan);
    printLatency("producer log() latency (sync mutex):", percentiles(r.latencyNs));
}

void checkLatencyBudget() {
    // Loose bound, only where timing means something: optimized and not sanitized.
#if defined(NDEBUG)
    if (kSanitized) {
        return;
    }
    Watchdog dog("latency budget", 120u);
    resetLogger();
    Logger& logger = Logger::instance();
    logger.startAsync({1u << 16});
    ProducerPlan plan;
    plan.threads = 4u;
    plan.messagesPerThread = 50000u;
    plan.flushEvery = 8192u;
    const Percentiles p = percentiles(runProducers(plan).latencyNs);
    logger.stopAsync();
    expectTrue(p.p50 < 2000u, "latency: async producer median < 2 us (optimized build)");
#endif
}

} // namespace

int main() {
    std::printf("fuse_core_b1_log_async_ring_gates (sanitized=%d)\n", kSanitized ? 1 : 0);
    testStressNoLossPerThreadOrder();
    testSteadyStateHeapWholeProcess();
    testForcedOverflowExactDropCount();
    testStopWhileLoggingKeepsOrder();
    testFatalFlushesAndIsSynchronous();
    testSynchronousLatencyBaseline();
    checkLatencyBudget();

    Logger::instance().stopAsync();
    Logger::instance().setSink(nullptr, nullptr);
    Logger::instance().setMinLevel(Level::Info);

    if (g_failures == 0) {
        std::printf("fuse_core_b1_log_async_ring_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_log_async_ring_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
