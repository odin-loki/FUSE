// Single-source compute kernel gates (fuse/compute_kernel/*, docs/compute-kernels.md):
//   - Backend parity: CpuReference vs CpuParallel bit-exact for integer and float kernels over
//     1D/2D/3D grids with partial edge workgroups (run_parity + compare_bitwise / compare_floats).
//   - CpuReference visits items in deterministic linear order (x fastest, then y, then z).
//   - CpuParallel launches make zero heap allocations in steady state (counted through this
//     binary's replaced global operator new) at 1/2/4 workers.
//   - Workgroup emulation (scratch + bulk-synchronous phases): tiled histogram and a three-launch
//     prefix scan (block scan -> scan of block sums -> add offsets) match std on both CPU backends.
//   - Every launch opens a profiler scope named after the kernel and records stats (items,
//     workgroups, backend, duration); GPU backends fall back / fail as documented; the Vulkan seam
//     calls a launch-supplied entry; invalid launches are rejected.
//   - LoadScale knob: parse / env / scaled counts / RAII override.

#include "compute_kernel_test_kernels.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/profiler/profiler.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

// ---- global heap counter (whole binary, every thread) ------------------------------------------

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};

#if defined(_WIN32)
void* testAlignedAlloc(std::size_t alignment, std::size_t size) { return _aligned_malloc(size, alignment); }
void testAlignedFree(void* ptr) { _aligned_free(ptr); }
#else
void* testAlignedAlloc(std::size_t alignment, std::size_t size) { return std::aligned_alloc(alignment, size); }
void testAlignedFree(void* ptr) { std::free(ptr); }
#endif
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
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
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
    if (void* p = testAlignedAlloc(align, rounded)) {
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
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::align_val_t) noexcept { testAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::align_val_t) noexcept { testAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    testAlignedFree(ptr);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    testAlignedFree(ptr);
}

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
namespace kt = fuse::kernel_test;
using kernel::Backend;
using kernel::Dim3;
using kernel::KernelLaunch;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void setWorkers(u32 workers) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
}

template <typename T>
kernel::Span<T> spanOf(std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

template <typename T>
kernel::Span<const T> constSpanOf(const std::vector<T>& v) {
    return kernel::Span<const T>{v.data(), static_cast<u32>(v.size())};
}

// ---- parity ------------------------------------------------------------------------------------

void testParity() {
    struct Shape {
        Dim3 grid;
        Dim3 workgroup;
    };
    const Shape shapes[] = {
        {kernel::extent1(1), {64, 1, 1}},        {kernel::extent1(1000), {64, 1, 1}},
        {kernel::extent2(97, 61), {8, 8, 1}},    {kernel::extent2(256, 256), {16, 16, 1}},
        {kernel::extent3(13, 9, 7), {4, 4, 4}},  {kernel::extent3(33, 1, 5), {32, 1, 1}},
    };
    for (u32 workers : {0u, 2u, 4u}) {
        setWorkers(workers);
        for (const Shape& s : shapes) {
            const u32 n = static_cast<u32>(s.grid.count());
            const KernelLaunch hashLaunch{"test_hash", s.grid, s.workgroup};
            std::vector<u32> a(n, 0xdeadbeefu);
            std::vector<u32> b(n, 0xcafef00du);
            const kt::HashParams pa{spanOf(a), 17u};
            const kt::HashParams pb{spanOf(b), 17u};
            const kernel::ParityReport hash = kernel::run_parity(
                Backend::CpuReference, Backend::CpuParallel, hashLaunch, kt::HashKernel{}, pa, pb, [&] {
                    return kernel::compare_bitwise(std::span<const u32>(a), std::span<const u32>(b));
                });
            expectTrue(hash.ok && hash.compared == n, "hash kernel: CpuReference == CpuParallel bit-exact");
            expectTrue(hash.backend_a == Backend::CpuReference && hash.backend_b == Backend::CpuParallel,
                       "parity report names the backends that ran");
            bool direct = true;
            for (u32 z = 0; z < s.grid.z; ++z) {
                for (u32 y = 0; y < s.grid.y; ++y) {
                    for (u32 x = 0; x < s.grid.x; ++x) {
                        const u32 i = x + y * s.grid.x + z * s.grid.x * s.grid.y;
                        direct = direct && a[i] == kt::hash_u32(x ^ kt::hash_u32(y ^ kt::hash_u32(z + 17u)));
                    }
                }
            }
            expectTrue(direct, "hash kernel: every item written exactly with its own global index");

            const KernelLaunch waveLaunch{"test_wave", s.grid, s.workgroup};
            std::vector<f32> fa(n, -1.f);
            std::vector<f32> fb(n, -2.f);
            const kernel::ParityReport wave = kernel::run_parity(
                Backend::CpuReference, Backend::CpuParallel, waveLaunch, kt::WaveKernel{},
                kt::WaveParams{spanOf(fa), 7.5f}, kt::WaveParams{spanOf(fb), 7.5f}, [&] {
                    return kernel::compare_bitwise(std::span<const f32>(fa), std::span<const f32>(fb));
                });
            expectTrue(wave.ok, "float kernel: CpuReference == CpuParallel bit-exact");
        }
    }

    // The float comparator itself: tolerance accepts small error, flags larger and NaN mismatches.
    const std::vector<f32> ref{1.f, 2.f, 3.f, 4.f};
    std::vector<f32> other{1.f, 2.000001f, 3.f, 4.f};
    kernel::ParityReport close = kernel::compare_floats(std::span<const f32>(ref), std::span<const f32>(other),
                                                        kernel::Tolerance{1e-5, 0.0});
    expectTrue(close.ok && close.max_abs_error > 0.0, "compare_floats: within tolerance passes, error reported");
    other[2] = 3.1f;
    other[3] = std::numeric_limits<f32>::quiet_NaN();
    close = kernel::compare_floats(std::span<const f32>(ref), std::span<const f32>(other), kernel::Tolerance{1e-5, 0.0});
    expectTrue(!close.ok && close.mismatches == 2u && close.first_mismatch == 2u,
               "compare_floats: out-of-tolerance and NaN elements flagged");
}

// ---- deterministic ordering --------------------------------------------------------------------

void testReferenceOrder() {
    setWorkers(4);
    const KernelLaunch launch{"test_order", kernel::extent3(7, 5, 3), {4, 2, 2}};
    const u32 n = static_cast<u32>(launch.grid.count());
    for (int run = 0; run < 2; ++run) {
        std::vector<u32> order(n, ~0u);
        u32 cursor = 0;
        const kernel::LaunchResult r =
            kernel::launch(Backend::CpuReference, launch, kt::OrderKernel{}, kt::OrderParams{spanOf(order), &cursor});
        bool linear = r.ok && cursor == n;
        for (u32 i = 0; i < n && linear; ++i) {
            linear = order[i] == i;
        }
        expectTrue(linear, "CpuReference visits items in linear order (x, then y, then z) every run");
    }
}

// ---- workgroup emulation -----------------------------------------------------------------------

void testHistogram(Backend backend, u32 count) {
    std::vector<u32> values(count);
    for (u32 i = 0; i < count; ++i) {
        values[i] = kt::hash_u32(i * 2654435761u) % 997u; // uneven bin distribution
    }
    const u32 groups = kernel::div_up(count, kt::kHistogramBins);
    std::vector<u32> groupHist(static_cast<std::size_t>(groups) * kt::kHistogramBins, 0xffffffffu);
    std::vector<u32> hist(kt::kHistogramBins, 0u);
    const KernelLaunch launch{"test_tiled_histogram", kernel::extent1(count), {kt::kHistogramBins, 1, 1}};
    const kernel::LaunchResult r = kernel::launch(
        backend, launch, kt::TiledHistogramKernel{},
        kt::HistogramParams{constSpanOf(values), spanOf(groupHist), spanOf(hist)});

    std::vector<u32> expected(kt::kHistogramBins, 0u);
    std::vector<u32> expectedGroups(groupHist.size(), 0u);
    for (u32 i = 0; i < count; ++i) {
        ++expected[values[i] & 0xffu];
        ++expectedGroups[(i / kt::kHistogramBins) * kt::kHistogramBins + (values[i] & 0xffu)];
    }
    expectTrue(r.ok && r.backend == backend && r.workgroups == groups, "histogram launch ran on the requested backend");
    expectTrue(hist == expected, "tiled histogram (scratch atomics + global atomics) matches std count");
    expectTrue(groupHist == expectedGroups, "per-workgroup histograms match (padding threads excluded)");
}

void testPrefixScan(Backend backend, u32 count) {
    std::vector<u32> input(count);
    for (u32 i = 0; i < count; ++i) {
        input[i] = kt::hash_u32(i) & 0xffu;
    }
    const u32 groups = kernel::div_up(count, kt::kScanGroupSize);
    std::vector<u32> output(count, 0u);
    std::vector<u32> blockSums(groups, 0u);
    std::vector<u32> blockOffsets(groups, 0u);

    const KernelLaunch blockScan{"test_block_scan", kernel::extent1(count), {kt::kScanGroupSize, 1, 1}};
    const bool a = kernel::launch(backend, blockScan, kt::BlockScanKernel{},
                                  kt::BlockScanParams{constSpanOf(input), spanOf(output), spanOf(blockSums)})
                       .ok;
    // groups <= 256 here, so one workgroup scans the block sums.
    const KernelLaunch sumScan{"test_block_sum_scan", kernel::extent1(groups), {kt::kScanGroupSize, 1, 1}};
    const bool b = kernel::launch(backend, sumScan, kt::BlockScanKernel{},
                                  kt::BlockScanParams{constSpanOf(blockSums), spanOf(blockOffsets), {}})
                       .ok;
    const KernelLaunch addOffsets{"test_add_offsets", kernel::extent1(count), {128, 1, 1}};
    const bool c = kernel::launch(backend, addOffsets, kt::AddOffsetsKernel{},
                                  kt::AddOffsetsParams{spanOf(output), constSpanOf(blockOffsets)})
                       .ok;

    std::vector<u32> expected(count, 0u);
    std::exclusive_scan(input.begin(), input.end(), expected.begin(), 0u);
    expectTrue(a && b && c, "prefix scan launches succeed");
    expectTrue(output == expected, "workgroup prefix scan (phased Hillis-Steele) == std::exclusive_scan");
}

void testWorkgroupEmulation() {
    for (u32 workers : {0u, 4u}) {
        setWorkers(workers);
        for (Backend backend : {Backend::CpuReference, Backend::CpuParallel}) {
            testHistogram(backend, 256u * 40u);
            testHistogram(backend, 256u * 13u + 77u); // partial last workgroup
            testHistogram(backend, 5u);
            testPrefixScan(backend, 256u * 37u + 13u);
            testPrefixScan(backend, 256u);
            testPrefixScan(backend, 1u);
        }
    }
}

// ---- zero heap allocations ---------------------------------------------------------------------

void testZeroAllocations(u32 workers) {
    setWorkers(workers);
    constexpr u32 kItems = 64u * 1024u;
    std::vector<u32> hashOut(kItems);
    std::vector<u32> values(kItems);
    for (u32 i = 0; i < kItems; ++i) {
        values[i] = kt::hash_u32(i);
    }
    std::vector<u32> groupHist(static_cast<std::size_t>(kItems / kt::kHistogramBins) * kt::kHistogramBins);
    std::vector<u32> hist(kt::kHistogramBins);
    const KernelLaunch hashLaunch{"test_alloc_hash", kernel::extent2(256, kItems / 256), {16, 16, 1}};
    const KernelLaunch histLaunch{"test_alloc_histogram", kernel::extent1(kItems), {kt::kHistogramBins, 1, 1}};
    const kt::HashParams hashParams{spanOf(hashOut), 3u};
    const kt::HistogramParams histParams{constSpanOf(values), spanOf(groupHist), spanOf(hist)};

    bool ok = true;
    const auto round = [&] {
        ok = kernel::launch(Backend::CpuParallel, hashLaunch, kt::HashKernel{}, hashParams).ok && ok;
        ok = kernel::launch(Backend::CpuParallel, histLaunch, kt::TiledHistogramKernel{}, histParams).ok && ok;
        ok = kernel::launch(Backend::Auto, hashLaunch, kt::HashKernel{}, hashParams).ok && ok;
    };
    for (u32 i = 0; i < 200; ++i) { // warm-up: scheduler pools, profiler thread buffers
        round();
    }
    constexpr u32 kRounds = 500;
    const u64 before = g_heapAllocations.load(std::memory_order_acquire);
    for (u32 i = 0; i < kRounds; ++i) {
        round();
    }
    const u64 allocs = g_heapAllocations.load(std::memory_order_acquire) - before;
    std::printf("  workers=%u: heap allocations over %u CpuParallel launches: %llu\n", workers, kRounds * 3u,
                static_cast<unsigned long long>(allocs));
    expectTrue(ok, "steady-state CpuParallel launches succeed");
    char label[96];
    std::snprintf(label, sizeof(label), "CpuParallel launches make 0 heap allocations in steady state (%u workers)",
                  workers);
    expectTrue(allocs == 0u, label);
}

// ---- profiler + stats + backend resolution -----------------------------------------------------

u32 g_fakeVulkanCalls = 0;
u32 g_fakeVulkanItems = 0;

bool fakeVulkanEntry(const KernelLaunch& launch, const void* body, const void* params, void* stream,
                     bool synchronize) {
    ++g_fakeVulkanCalls;
    g_fakeVulkanItems = static_cast<u32>(launch.grid.count());
    return body != nullptr && params != nullptr && stream == &g_fakeVulkanCalls && synchronize;
}

void testProfilingAndStats() {
    setWorkers(2);
    kernel::reset_kernel_stats();
    fuse::profiler::setEnabled(true);
    fuse::profiler::reset();

    std::vector<u32> out(300);
    const kt::HashParams params{spanOf(out), 1u};
    const KernelLaunch launch{"test_profiled_kernel", kernel::extent2(20, 15), {8, 8, 1}};
    const kernel::LaunchResult r1 = kernel::launch(Backend::CpuReference, launch, kt::HashKernel{}, params);
    const kernel::LaunchResult r2 = kernel::launch(Backend::CpuParallel, launch, kt::HashKernel{}, params);
    expectTrue(r1.ok && r2.ok && r1.items == 300u && r1.workgroups == 3u * 2u, "launch result: items / workgroups");

#if defined(FUSE_NO_PROFILER) && FUSE_NO_PROFILER
    // Shipping compiles FUSE_PROFILE_SCOPE out, so launches must record no profiler events at all.
    expectTrue(fuse::profiler::countEventsByName("test_profiled_kernel") == 0u,
               "shipping: kernel launches record no profiler scopes (FUSE_NO_PROFILER)");
#else
    expectTrue(fuse::profiler::countEventsByName("test_profiled_kernel") == 4u,
               "each launch opens a profiler scope named after the kernel (begin + end x 2)");
#endif

    kernel::KernelStats stats{};
    const bool found = kernel::find_kernel_stats("test_profiled_kernel", stats);
    expectTrue(found && stats.launches == 2u && stats.items == 600u && stats.workgroups == 12u,
               "stats registry aggregates launches / items / workgroups per kernel name");
    expectTrue(stats.launches_by_backend[static_cast<u32>(Backend::CpuReference)] == 1u &&
                   stats.launches_by_backend[static_cast<u32>(Backend::CpuParallel)] == 1u &&
                   stats.last_backend == Backend::CpuParallel,
               "stats registry records the backend of every launch");
    expectTrue(stats.total_ns >= stats.max_ns && stats.max_ns >= stats.min_ns && stats.last_ns == r2.duration_ns,
               "stats registry records durations");
    const kernel::LaunchRecord last = kernel::last_launch();
    expectTrue(last.ok && std::strcmp(last.name, "test_profiled_kernel") == 0 && last.items == 300u &&
                   last.backend == Backend::CpuParallel,
               "last_launch() returns the most recent record");

    // GPU requests without a device entry fall back to CpuParallel (or fail when fallback is off).
    const kernel::LaunchResult cuda = kernel::launch(Backend::Cuda, launch, kt::HashKernel{}, params);
    expectTrue(cuda.ok && cuda.backend == Backend::CpuParallel, "Cuda without entry/device falls back to CpuParallel");
    kernel::LaunchOptions strict{};
    strict.allow_fallback = false;
    const kernel::LaunchResult cudaStrict = kernel::launch(Backend::Cuda, launch, kt::HashKernel{}, params, strict);
    expectTrue(!cudaStrict.ok, "Cuda with fallback disabled fails when unavailable");
    const kernel::LaunchResult autoBackend = kernel::launch(Backend::Auto, launch, kt::HashKernel{}, params);
    expectTrue(autoBackend.ok && autoBackend.backend == Backend::CpuParallel, "Auto without a device entry -> CpuParallel");
#if !defined(FUSE_HAS_CUDA)
    expectTrue(!kernel::backend_available(Backend::Cuda), "Cuda unavailable in a non-CUDA build");
#endif
    expectTrue(kernel::backend_available(Backend::CpuReference) && kernel::backend_available(Backend::CpuParallel),
               "CPU backends always available");

    // Vulkan seam: a launch-supplied entry receives the typed body/params and the command buffer.
    kernel::LaunchOptions vk{};
    vk.vulkan = &fakeVulkanEntry;
    vk.stream = &g_fakeVulkanCalls;
    const kernel::LaunchResult vkResult = kernel::launch(Backend::VulkanCompute, launch, kt::HashKernel{}, params, vk);
    expectTrue(vkResult.ok && vkResult.backend == Backend::VulkanCompute && g_fakeVulkanCalls == 1u &&
                   g_fakeVulkanItems == 300u,
               "VulkanCompute seam dispatches through LaunchOptions::vulkan");
    const kernel::LaunchResult vkFallback = kernel::launch(Backend::VulkanCompute, launch, kt::HashKernel{}, params);
    expectTrue(vkFallback.ok && vkFallback.backend == Backend::CpuParallel, "VulkanCompute without entry falls back");

    // Invalid launches are rejected (and still recorded as failures).
    const KernelLaunch noName{nullptr, kernel::extent1(4), {4, 1, 1}};
    const KernelLaunch zeroGroup{"test_invalid", kernel::extent1(4), {0, 1, 1}};
    const KernelLaunch hugeGroup{"test_invalid", kernel::extent1(4), {64, 64, 1}};
    expectTrue(!kernel::launch(Backend::CpuReference, noName, kt::HashKernel{}, params).ok &&
                   !kernel::launch(Backend::CpuReference, zeroGroup, kt::HashKernel{}, params).ok &&
                   !kernel::launch(Backend::CpuReference, hugeGroup, kt::HashKernel{}, params).ok,
               "unnamed / empty / oversized workgroup launches rejected");
    kernel::KernelStats invalid{};
    expectTrue(kernel::find_kernel_stats("test_invalid", invalid) && invalid.failed_launches == 2u,
               "rejected launches are recorded as failed");
    const KernelLaunch empty{"test_empty", Dim3{0, 1, 1}, {64, 1, 1}};
    expectTrue(kernel::launch(Backend::CpuParallel, empty, kt::HashKernel{}, params).ok, "empty grid is a no-op");

    kernel::reset_kernel_stats();
    expectTrue(kernel::kernel_stats_count() == 0u && kernel::total_launch_count() == 0u, "stats reset");
}

// ---- load scale --------------------------------------------------------------------------------

void testLoadScale() {
    expectTrue(kernel::load_scale() == kernel::LoadScale{}, "default load scale is 1 on every axis");
    kernel::LoadScale s{};
    expectTrue(kernel::parse_load_scale("res=0.5,objects=4,probes=2,lights=8", s) && s.resolution == 0.5f &&
                   s.objects == 4.f && s.probes == 2.f && s.lights == 8.f,
               "parse_load_scale: key=value list");
    expectTrue(kernel::parse_load_scale("objects=3", s) && s.objects == 3.f && s.resolution == 0.5f,
               "parse_load_scale: unmentioned axes keep their value");
    expectTrue(kernel::parse_load_scale("2", s) && s == kernel::LoadScale{2.f, 2.f, 2.f, 2.f},
               "parse_load_scale: single number scales every axis");
    const kernel::LoadScale before = s;
    expectTrue(!kernel::parse_load_scale("bogus=1", s) && !kernel::parse_load_scale("res=", s) &&
                   !kernel::parse_load_scale("res=-1", s) && !kernel::parse_load_scale("", s) && s == before,
               "parse_load_scale: bad input rejected, value untouched");
    expectTrue(kernel::scaled_count(100, 0.25f) == 25u && kernel::scaled_count(3, 0.01f) == 1u &&
                   kernel::scaled_count(0, 5.f) == 0u && kernel::scaled_count(10, 0.f) == 0u &&
                   kernel::scaled_extent(1920, 0.5f) == 960u && kernel::scaled_extent(10, 0.f) == 1u,
               "scaled_count / scaled_extent rounding and clamps");
    {
        const kernel::ScopedLoadScale scoped(kernel::LoadScale{0.5f, 2.f, 1.f, 4.f});
        expectTrue(kernel::load_scale().lights == 4.f, "ScopedLoadScale applies");
    }
    expectTrue(kernel::load_scale() == kernel::LoadScale{}, "ScopedLoadScale restores");
}

} // namespace

int main() {
    std::printf("Compute kernel gates (single-source CPU backends)\n");
    testParity();
    testReferenceOrder();
    testWorkgroupEmulation();
    for (u32 workers : {1u, 2u, 4u}) {
        testZeroAllocations(workers);
    }
    testProfilingAndStats();
    testLoadScale();
    fuse::jobs::JobScheduler::instance().shutdown();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d compute kernel gate check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("All compute kernel gates passed\n");
    return EXIT_SUCCESS;
}
