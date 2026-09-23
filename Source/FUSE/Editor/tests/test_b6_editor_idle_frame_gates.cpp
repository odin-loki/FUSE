// B6 gates — rows "No frame spikes from editor on non-interactive frames — verified over 10,000
// frames" and "Memory overhead of editor layer < 256MB".
//
// Scope: the HEADLESS editor layer, i.e. `fuse::editor::EditorHost::gameTick()` — the per-frame loop
// the Qt shell's GameLoopThread drives (command drain, property command stack, runtime viewport hook:
// editor->runtime entity mirror, viewport panel tick, play-session gating). No Qt widgets are
// created, so Qt draw / widget memory is NOT covered here. The test is only registered in builds
// without the Vulkan backend, where the runtime viewport hook does not boot the hybrid GPU renderer —
// i.e. what is timed is editor-layer work, not renderer frames.
//
//   * 10,000 idle frames (no UI input, no commands, not playing) after warm-up over a 1,000-entity
//     edit scene: zero heap allocations on any thread in every frame; no frame whose editor CPU time
//     (CLOCK_THREAD_CPUTIME_ID, immune to preemption by other processes) exceeds 3x the median, taking
//     per frame index the minimum over >= 3 identically prepared runs to filter OS noise (see below).
//     Timing is enforced in Release, non-instrumented builds only (fuse::core::timingBudgetsEnforced);
//     wall-clock stats are printed alongside.
//   * Memory: RSS and live-heap growth of the editor layer (EditorHost + its edit scene + 10k frames)
//     over a runtime-only baseline (runtime ECS registry + runtime scene holding the same entities)
//     must stay < 256 MiB; the heap must not grow across the 10k idle frames (no leak / creep).
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/scene/scene.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <malloc.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<unsigned long> g_allocations{0};
std::atomic<long long> g_liveHeapBytes{0};

std::size_t usableSize(void* p) {
#if defined(__linux__)
    return p != nullptr ? malloc_usable_size(p) : 0u;
#elif defined(_WIN32)
    return p != nullptr ? _msize(p) : 0u;
#else
    (void)p;
    return 0u;
#endif
}

void* countedAlloc(std::size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1u, std::memory_order_relaxed);
    }
    g_liveHeapBytes.fetch_add(static_cast<long long>(usableSize(p)), std::memory_order_relaxed);
    return p;
}

void countedFree(void* p) {
    if (p == nullptr) {
        return;
    }
    g_liveHeapBytes.fetch_sub(static_cast<long long>(usableSize(p)), std::memory_order_relaxed);
    std::free(p);
}

void* countedAlignedAlloc(std::size_t size, std::align_val_t align) {
    const std::size_t a = std::max<std::size_t>(static_cast<std::size_t>(align), sizeof(void*));
    void* p = nullptr;
#if defined(_WIN32)
    // Windows CRT: no posix_memalign, and msvcrt has no _aligned_msize. Keep the byte count in an
    // `a`-byte header in front of the _aligned_malloc block; countedAlignedFree reads it back.
    const std::size_t bytes = size == 0 ? a : size;
    auto* base = static_cast<unsigned char*>(_aligned_malloc(bytes + a, a));
    if (base == nullptr) {
        throw std::bad_alloc();
    }
    *reinterpret_cast<std::size_t*>(base) = bytes;
    p = base + a;
    const std::size_t usable = bytes;
#else
    if (posix_memalign(&p, a, size == 0 ? a : size) != 0) {
        throw std::bad_alloc();
    }
    const std::size_t usable = usableSize(p);
#endif
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1u, std::memory_order_relaxed);
    }
    g_liveHeapBytes.fetch_add(static_cast<long long>(usable), std::memory_order_relaxed);
    return p;
}

void countedAlignedFree(void* p, std::align_val_t align) {
#if defined(_WIN32)
    if (p == nullptr) {
        return;
    }
    const std::size_t a = std::max<std::size_t>(static_cast<std::size_t>(align), sizeof(void*));
    unsigned char* base = static_cast<unsigned char*>(p) - a;
    g_liveHeapBytes.fetch_sub(static_cast<long long>(*reinterpret_cast<std::size_t*>(base)),
                              std::memory_order_relaxed);
    _aligned_free(base);
#else
    (void)align;
    countedFree(p);
#endif
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t align) { return countedAlignedAlloc(size, align); }
void* operator new[](std::size_t size, std::align_val_t align) { return countedAlignedAlloc(size, align); }
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t align) noexcept { countedAlignedFree(p, align); }
void operator delete[](void* p, std::align_val_t align) noexcept { countedAlignedFree(p, align); }
void operator delete(void* p, std::size_t, std::align_val_t align) noexcept { countedAlignedFree(p, align); }
void operator delete[](void* p, std::size_t, std::align_val_t align) noexcept { countedAlignedFree(p, align); }

namespace {

using fuse::f32;
using fuse::u32;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kEntities = 1000;
constexpr u32 kWarmupFrames = 120;
constexpr u32 kFrames = 10000;
/// Timing runs: at least kMinRuns; more (up to kMaxRuns) only while the per-index minimum still
/// shows a spike. A spike the editor causes recurs at its tick index in every run, so extra runs can
/// never hide it; they only strip OS noise that happened to hit the same index in all runs so far.
constexpr u32 kMinRuns = 3;
constexpr u32 kMaxRuns = 12;
constexpr double kSpikeFactor = 3.0;
constexpr long long kMiB = 1024ll * 1024ll;
constexpr long long kEditorBudgetBytes = 256ll * kMiB;

long long residentBytes() {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return -1;
    }
    long long sizePages = 0;
    long long residentPages = 0;
    const int read = std::fscanf(f, "%lld %lld", &sizePages, &residentPages);
    std::fclose(f);
    return read == 2 ? residentPages * static_cast<long long>(sysconf(_SC_PAGESIZE)) : -1;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
        return -1;
    }
    return static_cast<long long>(counters.WorkingSetSize); // resident set = working set
#else
    return -1;
#endif
}

long long nowNs(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

void populate(fuse::ecs::Registry& reg) {
    for (u32 i = 0; i < kEntities; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {static_cast<f32>(i % 32) * 2.f, 0.f, static_cast<f32>(i / 32) * 2.f, 1.f};
        reg.add(id, t);
        if ((i & 1u) == 0u) {
            fuse::ecs::SDFObject sdf{};
            sdf.type = fuse::ecs::SDFPrimitive::Sphere;
            sdf.params = {0.5f, 0.f, 0.f, 0.f};
            reg.add(id, sdf);
        } else {
            fuse::ecs::Mesh mesh{};
            mesh.aabb_min = {-0.5f, -0.5f, -0.5f, 0.f};
            mesh.aabb_max = {0.5f, 0.5f, 0.5f, 0.f};
            reg.add(id, mesh);
        }
    }
}

double percentile(std::vector<long long> v, double p) {
    std::sort(v.begin(), v.end());
    const std::size_t idx = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1)));
    return static_cast<double>(v[idx]);
}

struct IdleStats {
    u32 framesWithAllocations = 0;
    unsigned long totalAllocations = 0;
};

std::unique_ptr<fuse::editor::EditorHost> makeWarmHost() {
    auto host = std::make_unique<fuse::editor::EditorHost>();
    populate(host->editorScene().registry());
    host->runtimeViewport().requestResize(1280, 720);
    for (u32 i = 0; i < kWarmupFrames; ++i) {
        host->gameTick();
    }
    expectTrue(host->runtimeScene().entities().size() == kEntities,
               "warm-up mirrored every edit-scene entity into the runtime scene");
    expectTrue(!host->editorState().playing, "editor is idle (not playing)");
    return host;
}

/// kFrames idle ticks: per-frame editor thread CPU time, wall time and allocations (all threads).
IdleStats runIdleFrames(fuse::editor::EditorHost& host, std::vector<long long>& cpuNs, std::vector<long long>& wallNs) {
    IdleStats stats{};
    for (u32 frame = 0; frame < kFrames; ++frame) {
        g_allocations.store(0);
        g_counting.store(true);
        const long long c0 = nowNs(CLOCK_THREAD_CPUTIME_ID);
        const long long w0 = nowNs(CLOCK_MONOTONIC);
        host.gameTick();
        const long long w1 = nowNs(CLOCK_MONOTONIC);
        const long long c1 = nowNs(CLOCK_THREAD_CPUTIME_ID);
        g_counting.store(false);
        const unsigned long allocations = g_allocations.load();
        stats.totalAllocations += allocations;
        stats.framesWithAllocations += allocations != 0u ? 1u : 0u;
        cpuNs[frame] = c1 - c0;
        wallNs[frame] = w1 - w0;
    }
    return stats;
}

} // namespace

int main() {
    fuse::core::initialize();

    // --- Runtime-only baseline: the runtime's own ECS + scene holding the same content. ---
    const long long rss0 = residentBytes();
    const long long heap0 = g_liveHeapBytes.load();
    auto runtimeRegistry = std::make_unique<fuse::ecs::Registry>();
    runtimeRegistry->init();
    populate(*runtimeRegistry);
    auto runtimeScene = std::make_unique<fuse::scene::Scene>("RuntimeOnly");
    for (u32 i = 0; i < kEntities; ++i) {
        runtimeScene->addEntity("RuntimeEntity_" + std::to_string(i));
    }
    const long long rssRuntime = residentBytes();
    const long long heapRuntime = g_liveHeapBytes.load();

    // --- Editor layer on top (run 0 also provides the memory numbers). ---
    std::vector<std::vector<long long>> cpuRuns(1, std::vector<long long>(kFrames));
    std::vector<long long> wallNs(kFrames);
    auto host = makeWarmHost();
    const long long heapAfterWarmup = g_liveHeapBytes.load();
    const IdleStats stats0 = runIdleFrames(*host, cpuRuns[0], wallNs);
    const long long heapAfterFrames = g_liveHeapBytes.load();
    const long long rssEditor = residentBytes();

    // --- Allocations. ---
    std::printf("idle frames: %u, frames with allocations: %u, total allocations: %lu\n", kFrames,
                stats0.framesWithAllocations, stats0.totalAllocations);
    expectTrue(host->commandsAppliedLastTick() == 0u, "idle frames applied no commands");
    expectTrue(stats0.framesWithAllocations == 0u, "zero heap allocations (all threads) in every idle editor frame");
    std::printf("live heap across idle frames: %+lld bytes\n", heapAfterFrames - heapAfterWarmup);
    expectTrue(heapAfterFrames == heapAfterWarmup, "live heap unchanged across 10,000 idle frames (no creep)");

    // Memory figures are taken now; the extra timing runs below use fresh hosts.
    const long long rssOverhead = rssEditor - rssRuntime;
    const long long heapOverhead = heapAfterFrames - heapRuntime;
    host.reset();

    // --- Spikes. ---
    // An idle editor frame is a deterministic function of the tick index (no input, no commands),
    // so a spike *caused by the editor* (periodic work, rehash, lazy rebuild, ...) recurs at the same
    // tick index in every identically prepared run. OS noise (interrupts / cache effects charged to
    // the thread, heavy under machine load) does not. The gate therefore takes, per frame index, the
    // minimum editor CPU time over fresh hosts (kMinRuns, extended up to kMaxRuns while a spike
    // remains) and requires no frame of that series above 3x its median. Single-run stats are
    // printed for transparency.
    const auto spikeCount = [](const std::vector<long long>& v, double median) {
        return static_cast<long long>(
            std::count_if(v.begin(), v.end(), [&](long long ns) { return ns > kSpikeFactor * median; }));
    };
    std::vector<long long> cpuMin = cpuRuns[0];
    while (cpuRuns.size() < kMaxRuns &&
           (cpuRuns.size() < kMinRuns || spikeCount(cpuMin, percentile(cpuMin, 0.5)) != 0)) {
        cpuRuns.emplace_back(kFrames);
        auto again = makeWarmHost();
        const IdleStats stats = runIdleFrames(*again, cpuRuns.back(), wallNs);
        expectTrue(stats.framesWithAllocations == 0u, "zero allocations in every idle frame (repeat run)");
        for (u32 frame = 0; frame < kFrames; ++frame) {
            cpuMin[frame] = std::min(cpuMin[frame], cpuRuns.back()[frame]);
        }
    }
    const auto printStats = [&](const char* label, const std::vector<long long>& v) {
        const double median = percentile(v, 0.5);
        std::printf("%s: median %.2f us, p99 %.2f us, p99.9 %.2f us, max %.2f us, >3x median: %lld\n", label,
                    median / 1e3, percentile(v, 0.99) / 1e3, percentile(v, 0.999) / 1e3,
                    static_cast<double>(*std::max_element(v.begin(), v.end())) / 1e3, spikeCount(v, median));
    };
    printStats("editor CPU/frame run 0 (single run)", cpuRuns[0]);
    printStats("wall/frame last run (info)", wallNs);
    std::printf("timing runs: %zu\n", cpuRuns.size());
    printStats("editor CPU/frame min over runs (gated)", cpuMin);
    const long long gatedSpikes = spikeCount(cpuMin, percentile(cpuMin, 0.5));

    // Sensitivity: a deterministic editor spike (here: +4x median at every 1000th tick, in every run)
    // must survive the per-index minimum and be flagged at each injected index.
    // Needs a thread CPU clock finer than an idle frame: winpthreads' CLOCK_THREAD_CPUTIME_ID is
    // GetThreadTimes (scheduler-tick granularity, ~15.6 ms), so on Windows the per-frame minimum is
    // all zeros and a "4x median" spike is 0 — skip rather than report a meaningless failure.
    timespec cpuRes{};
    const bool cpuClockFine = clock_getres(CLOCK_THREAD_CPUTIME_ID, &cpuRes) == 0 && cpuRes.tv_sec == 0 &&
                              cpuRes.tv_nsec <= 100000 && percentile(cpuMin, 0.5) > 0.0;
    if (!cpuClockFine) {
        std::printf("SKIP spike sensitivity: thread CPU clock resolution %lld ns is too coarse for per-frame timing\n",
                    static_cast<long long>(cpuRes.tv_sec) * 1000000000ll + cpuRes.tv_nsec);
    } else {
        const double median = percentile(cpuMin, 0.5);
        std::vector<long long> injectedMin(kFrames);
        for (u32 frame = 0; frame < kFrames; ++frame) {
            long long m = -1;
            for (const std::vector<long long>& run : cpuRuns) {
                const long long ns =
                    run[frame] + (frame % 1000u == 500u ? static_cast<long long>(4.0 * median) : 0ll);
                m = m < 0 ? ns : std::min(m, ns);
            }
            injectedMin[frame] = m;
        }
        const double injectedMedian = percentile(injectedMin, 0.5);
        int flagged = 0;
        for (u32 frame = 500; frame < kFrames; frame += 1000) {
            flagged += injectedMin[frame] > kSpikeFactor * injectedMedian ? 1 : 0;
        }
        expectTrue(flagged == 10, "sensitivity: all 10 injected deterministic spikes are flagged by the min-over-runs metric");
    }
#if defined(NDEBUG)
    constexpr bool kRelease = true;
#else
    constexpr bool kRelease = false;
#endif
    if (kRelease && fuse::core::timingBudgetsEnforced()) {
        expectTrue(gatedSpikes == 0, "no idle editor frame above 3x the median editor CPU time over 10,000 frames");
    } else {
        std::printf("SKIP spike budget (debug or instrumented build) — stats above are informational\n");
    }

    // --- Memory. ---
    std::printf("runtime-only: RSS +%.1f MiB, heap +%.1f MiB over process start\n",
                static_cast<double>(rssRuntime - rss0) / kMiB, static_cast<double>(heapRuntime - heap0) / kMiB);
    std::printf("editor layer overhead: RSS +%.1f MiB, live heap +%.1f MiB (budget 256 MiB)\n",
                static_cast<double>(rssOverhead) / kMiB, static_cast<double>(heapOverhead) / kMiB);
    expectTrue(rssEditor > 0 && rssRuntime > 0, "RSS readable");
    expectTrue(rssOverhead < kEditorBudgetBytes, "editor layer RSS overhead < 256 MiB");
    expectTrue(heapOverhead < kEditorBudgetBytes, "editor layer live-heap overhead < 256 MiB");

    runtimeScene.reset();
    runtimeRegistry.reset();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_idle_frame_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_idle_frame_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
