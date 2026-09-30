// UNI-VFS-1 I/O lane gates (fuse::io::VirtualFileSystem):
//   - priority ordering (Critical > High > Normal > Background, FIFO inside a class)
//   - cancel before read (queued read dropped) and during read / decode (result discarded)
//   - suspend / resume on a synthetic PowerState::Background event and on app visibility
//   - per-frame read budget respected (and never starves an oversized read)
//   - concurrent mount / unmount / resolve stress (run under the TSan build for race detection)
//   - decode jobs run on JobScheduler workers with per-thread scratch reset per job
//   - zero heap allocations per steady-state callback/decode read once the lane and scratch are warm
//   - synchronous reads implemented on top of the lane (bypass suspend and budget)
//   - one or two dedicated I/O threads

#include <fuse/core/temp_path.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/power.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

// ---- global heap counter (whole binary, every thread) ------------------------------------------

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

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};
} // namespace

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
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    fuseTestAlignedFree(ptr);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    fuseTestAlignedFree(ptr);
}

namespace {

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::io::CancelResult;
using fuse::io::CompletedLoad;
using fuse::io::DecodeScratch;
using fuse::io::IoLaneConfig;
using fuse::io::IoPriority;
using fuse::io::LoadId;
using fuse::io::LoadRequest;
using fuse::io::MountKind;
using fuse::io::VirtualFileSystem;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

template <typename Pred>
bool waitUntil(Pred pred, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

/// Per-process directory so concurrent runs never truncate each other's files.
const std::string& laneDir() {
    static const std::string dir = fuse::test::makeUniqueTempDir("fuse_io_lane").generic_string();
    return dir;
}

void writeFile(const std::string& name, u32 size, char fill = 'x') {
    std::filesystem::create_directories(laneDir());
    std::ofstream out(laneDir() + "/" + name, std::ios::binary | std::ios::trunc);
    const std::string payload(size, fill);
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void mountLane(VirtualFileSystem& vfs) {
    vfs.mount(MountKind::Game, laneDir(), "/lane");
}

/// Blocks the I/O thread in onReadBegin for one chosen load id until released.
struct ReadGate {
    std::atomic<LoadId> target{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<u64> ioThreadHash{0};

    std::function<void(LoadId)> hook() {
        return [this](LoadId id) {
            ioThreadHash.store(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            if (id != target.load()) {
                return;
            }
            entered.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        };
    }
};

// ---- tests -------------------------------------------------------------------------------------

void testPriorityOrdering() {
    writeFile("prio.bin", 16);
    VirtualFileSystem vfs;
    mountLane(vfs);
    vfs.suspend();

    std::mutex mutex;
    std::vector<LoadId> order;
    auto submit = [&](IoPriority priority) {
        LoadRequest request;
        request.priority = priority;
        request.onComplete = [&](CompletedLoad& load) {
            expectTrue(load.success && load.asset.bytes.size() == 16u, "priority load reads payload");
            const std::lock_guard<std::mutex> lock(mutex);
            order.push_back(load.id);
        };
        return vfs.submitLoad("/lane/prio.bin", std::move(request));
    };

    const LoadId b1 = submit(IoPriority::Background);
    const LoadId n1 = submit(IoPriority::Normal);
    const LoadId h1 = submit(IoPriority::High);
    const LoadId c1 = submit(IoPriority::Critical);
    const LoadId b2 = submit(IoPriority::Background);
    const LoadId n2 = submit(IoPriority::Normal);
    const LoadId h2 = submit(IoPriority::High);
    const LoadId c2 = submit(IoPriority::Critical);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expectTrue(vfs.laneStats().queued == 8u, "suspended lane keeps every read queued");
    {
        const std::lock_guard<std::mutex> lock(mutex);
        expectTrue(order.empty(), "suspended lane delivers nothing");
    }

    vfs.resume();
    expectTrue(vfs.waitIdle(5000), "priority batch completes after resume");

    const std::vector<LoadId> expected = {c1, c2, h1, h2, n1, n2, b1, b2};
    const std::lock_guard<std::mutex> lock(mutex);
    expectTrue(order == expected, "reads served Critical > High > Normal > Background, FIFO per class");
}

void testCancelBeforeRead() {
    writeFile("cancel_q.bin", 8);
    VirtualFileSystem vfs;
    mountLane(vfs);
    vfs.suspend();

    const LoadId a = vfs.submitLoadAsync("/lane/cancel_q.bin");
    const LoadId b = vfs.submitLoadAsync("/lane/cancel_q.bin", IoPriority::High);
    const LoadId c = vfs.submitLoadAsync("/lane/cancel_q.bin");

    expectTrue(vfs.cancel(b) == CancelResult::DroppedQueued, "cancel of a queued read drops it");
    expectTrue(vfs.cancel(b) == CancelResult::NotFound, "second cancel finds nothing");
    expectTrue(vfs.laneStats().queued == 2u, "dropped read leaves the queue");

    vfs.resume();
    expectTrue(vfs.waitIdle(5000), "remaining reads complete");
    const std::vector<LoadId> order = vfs.peekCompletedLoadOrder();
    expectTrue(order.size() == 2u && order[0] == a && order[1] == c, "cancelled read never delivered");
    expectTrue(vfs.laneStats().cancelledQueued == 1u, "stats count the dropped read");
    expectTrue(vfs.laneStats().readsIssued == 2u, "cancelled read was never issued");

    expectTrue(vfs.cancel(a) == CancelResult::DiscardedCompleted, "cancel removes an undrained result");
    expectTrue(vfs.completedLoadCount() == 1u, "discarded result left the drain queue");
}

void testCancelDuringReadAndDecode() {
    writeFile("cancel_f.bin", 32);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    VirtualFileSystem vfs;
    mountLane(vfs);
    ReadGate gate;
    gate.target = ~LoadId{0};
    IoLaneConfig config;
    config.onReadBegin = gate.hook();
    vfs.configureLane(config);
    // Pre-reserve the id the next submit will receive so the hook blocks on it.
    std::atomic<bool> delivered{false};

    // Cancel while the I/O thread is inside the read.
    {
        vfs.suspend();
        LoadRequest request;
        request.onComplete = [&](CompletedLoad&) { delivered.store(true); };
        const LoadId id = vfs.submitLoad("/lane/cancel_f.bin", std::move(request));
        gate.target = id;
        vfs.resume();
        expectTrue(waitUntil([&] { return gate.entered.load(); }), "read reached the I/O thread");
        expectTrue(vfs.cancel(id) == CancelResult::DiscardInFlight, "cancel during read discards the result");
        gate.release = true;
        expectTrue(vfs.waitIdle(5000), "cancelled in-flight read retires");
        expectTrue(!delivered.load(), "cancelled in-flight read is never delivered");
        expectTrue(vfs.laneStats().discardedInFlight == 1u, "stats count the discarded read");
    }

    // Cancel while the decode job runs on a worker.
    {
        std::atomic<bool> decodeEntered{false};
        std::atomic<bool> decodeRelease{false};
        delivered = false;
        LoadRequest request;
        request.decode = [&](CompletedLoad&, DecodeScratch&) {
            decodeEntered = true;
            while (!decodeRelease.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        };
        request.onComplete = [&](CompletedLoad&) { delivered.store(true); };
        const LoadId id = vfs.submitLoad("/lane/cancel_f.bin", std::move(request));
        expectTrue(waitUntil([&] { return decodeEntered.load(); }), "decode job started");
        expectTrue(vfs.cancel(id) == CancelResult::DiscardInFlight, "cancel during decode discards the result");
        decodeRelease = true;
        expectTrue(vfs.waitIdle(5000), "cancelled decode retires");
        expectTrue(!delivered.load(), "cancelled decode is never delivered");
        expectTrue(vfs.laneStats().discardedInFlight == 2u, "stats count the discarded decode");
    }

    vfs.configureLane(IoLaneConfig{});
    scheduler.shutdown();
}

void testPowerStateSuspendResume() {
    writeFile("power.bin", 24);
    fuse::platform::setPowerState(fuse::platform::PowerState::Normal);
    VirtualFileSystem vfs;
    mountLane(vfs);
    ReadGate gate;
    gate.target = ~LoadId{0};
    IoLaneConfig config;
    config.onReadBegin = gate.hook();
    vfs.configureLane(config);

    std::atomic<u32> deliveredA{0};
    std::atomic<u32> deliveredB{0};
    vfs.suspend();
    LoadRequest requestA;
    requestA.onComplete = [&](CompletedLoad& load) {
        if (load.success) {
            deliveredA.fetch_add(1u);
        }
    };
    const LoadId a = vfs.submitLoad("/lane/power.bin", std::move(requestA));
    gate.target = a;
    vfs.resume();
    expectTrue(waitUntil([&] { return gate.entered.load(); }), "read A in flight before Background");

    fuse::platform::setPowerState(fuse::platform::PowerState::Background);
    expectTrue(vfs.isSuspended(), "PowerState::Background suspends the lane");

    LoadRequest requestB;
    requestB.onComplete = [&](CompletedLoad& load) {
        if (load.success) {
            deliveredB.fetch_add(1u);
        }
    };
    vfs.submitLoad("/lane/power.bin", std::move(requestB));
    gate.release = true;

    expectTrue(waitUntil([&] { return deliveredA.load() == 1u; }), "in-flight read finishes while suspended");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expectTrue(deliveredB.load() == 0u, "queued read stays queued while Background");
    expectTrue(vfs.laneStats().queued == 1u, "suspended lane still holds the queued read");

    fuse::platform::setPowerState(fuse::platform::PowerState::Normal);
    expectTrue(!vfs.isSuspended(), "leaving Background resumes the lane");
    expectTrue(vfs.waitIdle(5000) && deliveredB.load() == 1u, "queued read completes after resume");

    // App visibility routes through the same power state.
    fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Background);
    expectTrue(vfs.isSuspended(), "app Background suspends the lane");
    fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Foreground);
    expectTrue(!vfs.isSuspended(), "app Foreground resumes the lane");

    // followPowerState = false ignores the platform state.
    vfs.configureLane(IoLaneConfig{});
    IoLaneConfig ignore;
    ignore.followPowerState = false;
    vfs.configureLane(ignore);
    fuse::platform::setPowerState(fuse::platform::PowerState::Background);
    expectTrue(!vfs.isSuspended(), "followPowerState=false keeps the lane running");
    fuse::platform::setPowerState(fuse::platform::PowerState::Normal);
}

void testFrameBudget() {
    for (u32 index = 0; index < 5u; ++index) {
        writeFile("budget_" + std::to_string(index) + ".bin", 40);
    }
    writeFile("budget_big.bin", 300);

    VirtualFileSystem vfs;
    mountLane(vfs);
    IoLaneConfig config;
    config.frameReadBudgetBytes = 100u;
    vfs.configureLane(config);

    std::atomic<u32> delivered{0};
    std::atomic<bool> overBudget{false};
    for (u32 index = 0; index < 5u; ++index) {
        LoadRequest request;
        request.onComplete = [&](CompletedLoad&) {
            if (vfs.laneStats().bytesReadThisFrame > 100u) {
                overBudget = true;
            }
            delivered.fetch_add(1u);
        };
        vfs.submitLoad("/lane/budget_" + std::to_string(index) + ".bin", std::move(request));
    }

    expectTrue(waitUntil([&] { return delivered.load() == 2u && vfs.laneStats().budgetStalled; }),
               "frame 0 reads two 40-byte files, then stalls on the budget");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expectTrue(delivered.load() == 2u, "no further reads inside the exhausted frame");
    expectTrue(vfs.laneStats().bytesReadThisFrame == 80u, "frame 0 read 80 of 100 bytes");

    // Synchronous reads bypass the budget.
    std::vector<u8> syncBytes;
    expectTrue(vfs.readFileSync("/lane/budget_0.bin", syncBytes) && syncBytes.size() == 40u,
               "readFileSync bypasses a stalled budget");

    vfs.advanceFrame();
    expectTrue(waitUntil([&] { return delivered.load() == 4u && vfs.laneStats().budgetStalled; }),
               "frame 1 reads two more");
    vfs.advanceFrame();
    expectTrue(waitUntil([&] { return delivered.load() == 5u; }), "frame 2 reads the last file");
    expectTrue(vfs.waitIdle(5000), "budgeted lane drains");
    expectTrue(!overBudget.load(), "bytes read per frame never exceed the budget");

    // An oversized read is issued as the first read of a frame (no starvation).
    std::atomic<bool> bigDone{false};
    LoadRequest big;
    big.onComplete = [&](CompletedLoad& load) { bigDone = load.success && load.asset.bytes.size() == 300u; };
    vfs.submitLoad("/lane/budget_big.bin", std::move(big));
    expectTrue(waitUntil([&] { return vfs.laneStats().budgetStalled; }), "oversized read waits for a fresh frame");
    vfs.advanceFrame();
    expectTrue(vfs.waitIdle(5000) && bigDone.load(), "oversized read completes as the first read of a frame");
}

void testConcurrentMountResolve() {
    VirtualFileSystem vfs;
    vfs.mount(MountKind::Game, "/phys/stable", "/stable");

    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> resolves{0};
    std::vector<std::thread> readers;
    for (u32 reader = 0; reader < 4u; ++reader) {
        readers.emplace_back([&, reader] {
            std::string physical;
            while (!stop.load(std::memory_order_relaxed)) {
                if (!vfs.resolve("/stable/a.bin", physical) || physical != "/phys/stable/a.bin") {
                    failures.fetch_add(1u);
                }
                const std::string dynamicPath = "/dyn" + std::to_string(reader) + "/x.bin";
                if (vfs.resolve(dynamicPath, physical) && physical.find("/x.bin") == std::string::npos) {
                    failures.fetch_add(1u);
                }
                (void)vfs.mountCount();
                resolves.fetch_add(2u, std::memory_order_relaxed);
            }
        });
    }

    std::thread writer([&] {
        for (u32 iteration = 0; iteration < 2000u; ++iteration) {
            const std::string prefix = "/dyn" + std::to_string(iteration % 4u);
            vfs.mount(MountKind::T3DLegacy, "/phys" + prefix, prefix);
            if (!vfs.unmount(prefix)) {
                failures.fetch_add(1u);
            }
        }
    });
    writer.join();
    stop = true;
    for (std::thread& thread : readers) {
        thread.join();
    }

    expectTrue(failures.load() == 0u, "resolve stays correct while mounts change");
    expectTrue(resolves.load() > 0u, "readers resolved concurrently");
    expectTrue(vfs.mountCount() == 1u, "mount/unmount churn leaves only the stable mount");
}

void testDecodeOnWorkersWithScratch() {
    writeFile("decode.bin", 64, 'd');
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    VirtualFileSystem vfs;
    mountLane(vfs);
    ReadGate gate; // records the I/O thread id, never blocks
    IoLaneConfig config;
    config.decodeScratchBytes = 4096u;
    config.onReadBegin = gate.hook();
    vfs.configureLane(config);

    std::atomic<u32> decodedOk{0};
    std::atomic<u32> scratchDirty{0};
    std::atomic<u32> onIoThread{0};
    for (u32 index = 0; index < 16u; ++index) {
        LoadRequest request;
        request.priority = IoPriority::High;
        request.decode = [&](CompletedLoad& load, DecodeScratch& scratch) {
            if (scratch.stats().usedBytes != 0u || scratch.stats().totalBytes != 4096u) {
                scratchDirty.fetch_add(1u);
            }
            if (std::hash<std::thread::id>{}(std::this_thread::get_id()) == gate.ioThreadHash.load()) {
                onIoThread.fetch_add(1u);
            }
            u32* sum = scratch.allocate<u32>(1);
            *sum = 0;
            for (u8 byte : load.asset.bytes) {
                *sum += byte;
            }
            // "Decode": replace the payload with its checksum.
            load.asset.bytes.assign(reinterpret_cast<const u8*>(sum), reinterpret_cast<const u8*>(sum) + sizeof(u32));
        };
        request.onComplete = [&](CompletedLoad& load) {
            u32 sum = 0;
            if (load.success && load.asset.bytes.size() == sizeof(u32)) {
                std::memcpy(&sum, load.asset.bytes.data(), sizeof(u32));
            }
            if (sum == 64u * static_cast<u32>('d')) {
                decodedOk.fetch_add(1u);
            }
        };
        vfs.submitLoad("/lane/decode.bin", std::move(request));
    }
    expectTrue(vfs.waitIdle(5000), "decode batch completes");
    expectTrue(decodedOk.load() == 16u, "decode jobs transform every payload");
    expectTrue(scratchDirty.load() == 0u, "decode scratch is reset and sized per job");
    expectTrue(onIoThread.load() == 0u, "decode runs on scheduler workers, not the I/O thread");
    expectTrue(vfs.laneStats().decodesRun == 16u, "stats count decode jobs");

    vfs.configureLane(IoLaneConfig{});
    scheduler.shutdown();
}

void testZeroAllocationSteadyState() {
    writeFile("steady.bin", 1024, 's');
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(1);

    VirtualFileSystem vfs;
    mountLane(vfs);
    IoLaneConfig config;
    config.decodeScratchBytes = 8192u;
    vfs.configureLane(config);

    const std::string path = "/lane/steady.bin";
    std::atomic<u32> delivered{0};
    std::atomic<u32> decoded{0};
    constexpr u32 kBatch = 8u;

    auto runBatch = [&](bool withDecode) {
        for (u32 index = 0; index < kBatch; ++index) {
            LoadRequest request;
            request.priority = IoPriority::Normal;
            if (withDecode) {
                request.decode = [counter = &decoded](CompletedLoad& load, DecodeScratch& scratch) {
                    u8* staging = scratch.allocate<u8>(static_cast<u32>(load.asset.bytes.size()));
                    if (staging != nullptr) {
                        std::memcpy(staging, load.asset.bytes.data(), load.asset.bytes.size());
                        counter->fetch_add(1u, std::memory_order_relaxed);
                    }
                };
            }
            request.onComplete = [counter = &delivered](CompletedLoad& load) {
                if (load.success && load.asset.bytes.size() == 1024u) {
                    counter->fetch_add(1u, std::memory_order_relaxed);
                }
            };
            vfs.submitLoad(path, std::move(request));
        }
        return vfs.waitIdle(5000);
    };

    // Warm: lane threads, slot pool, queues, slot byte buffers, worker scratch, scheduler rings.
    const std::uint64_t coldStart = g_heapAllocations.load();
    for (u32 warm = 0; warm < 4u; ++warm) {
        runBatch(false);
        runBatch(true);
    }
    expectTrue(g_heapAllocations.load() > coldStart, "heap counter observes the cold warm-up (counter is live)");

    delivered = 0;
    decoded = 0;
    const std::uint64_t before = g_heapAllocations.load();
    bool idle = true;
    for (u32 round = 0; round < 50u; ++round) {
        idle = runBatch(false) && idle;
        idle = runBatch(true) && idle;
    }
    const std::uint64_t allocations = g_heapAllocations.load() - before;

    expectTrue(idle, "steady-state batches complete");
    expectTrue(delivered.load() == 50u * 2u * kBatch, "every steady-state read delivered");
    expectTrue(decoded.load() == 50u * kBatch, "every steady-state decode used warm scratch");
    if (allocations != 0u) {
        std::fprintf(stderr, "steady-state heap allocations: %llu over %u reads\n",
                     static_cast<unsigned long long>(allocations), 50u * 2u * kBatch);
    }
    expectTrue(allocations == 0u, "zero heap allocations per steady-state read with warm scratch");

    vfs.configureLane(IoLaneConfig{});
    scheduler.shutdown();
}

void testReadFileSync() {
    writeFile("sync.bin", 77, 'q');
    VirtualFileSystem vfs;
    mountLane(vfs);

    std::vector<u8> bytes;
    expectTrue(vfs.readFileSync("/lane/sync.bin", bytes) && bytes.size() == 77u && bytes[0] == 'q',
               "readFileSync returns the whole file");
    expectTrue(!vfs.readFileSync("/nowhere/sync.bin", bytes) && bytes.empty(), "unresolved sync read fails");
    expectTrue(!vfs.readFileSync("/lane/missing.bin", bytes), "missing file sync read fails");

    vfs.suspend();
    expectTrue(vfs.readFileSync("/lane/sync.bin", bytes) && bytes.size() == 77u, "readFileSync ignores suspension");
    vfs.resume();

    // A lane callback may read synchronously (served inline on the I/O thread).
    std::atomic<bool> nestedOk{false};
    LoadRequest request;
    request.onComplete = [&](CompletedLoad&) {
        std::vector<u8> nested;
        nestedOk = vfs.readFileSync("/lane/sync.bin", nested) && nested.size() == 77u;
    };
    vfs.submitLoad("/lane/sync.bin", std::move(request));
    expectTrue(vfs.waitIdle(5000) && nestedOk.load(), "readFileSync from a lane callback does not deadlock");

    // Unresolved async loads still complete immediately (callback on the caller).
    std::atomic<bool> failedCallback{false};
    LoadRequest missing;
    missing.onComplete = [&](CompletedLoad& load) { failedCallback = !load.success; };
    vfs.submitLoad("/nowhere/x.bin", std::move(missing));
    expectTrue(failedCallback.load(), "unresolved async load reports failure synchronously");
}

void testTwoIoThreads() {
    for (u32 index = 0; index < 8u; ++index) {
        writeFile("multi_" + std::to_string(index) + ".bin", 100u + index);
    }
    VirtualFileSystem vfs;
    mountLane(vfs);
    IoLaneConfig config;
    config.ioThreadCount = 5u; // clamped to 2
    vfs.configureLane(config);
    expectTrue(vfs.laneConfig().ioThreadCount == 2u, "I/O thread count clamps to 2");

    std::atomic<u32> correct{0};
    for (u32 index = 0; index < 64u; ++index) {
        const u32 file = index % 8u;
        LoadRequest request;
        request.priority = static_cast<IoPriority>(index % 4u);
        request.onComplete = [&correct, file](CompletedLoad& load) {
            if (load.success && load.asset.bytes.size() == 100u + file) {
                correct.fetch_add(1u);
            }
        };
        vfs.submitLoad("/lane/multi_" + std::to_string(file) + ".bin", std::move(request));
    }
    expectTrue(vfs.waitIdle(5000), "two-thread lane drains");
    expectTrue(correct.load() == 64u, "two I/O threads deliver every payload intact");
    expectTrue(vfs.laneStats().ioThreadCount == 2u, "lane runs two dedicated I/O threads");
}

} // namespace

int main() {
    testPriorityOrdering();
    testCancelBeforeRead();
    testCancelDuringReadAndDecode();
    testPowerStateSuspendResume();
    testFrameBudget();
    testConcurrentMountResolve();
    testDecodeOnWorkersWithScratch();
    testZeroAllocationSteadyState();
    testReadFileSync();
    testTwoIoThreads();

    std::error_code ec;
    std::filesystem::remove_all(laneDir(), ec);

    if (g_failures == 0) {
        std::printf("fuse_core io lane tests: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core io lane tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
