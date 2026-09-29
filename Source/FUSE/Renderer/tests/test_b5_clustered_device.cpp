// B5.4 clustered cull + deferred shade device gate / benchmark (fuse/renderer/lighting/clustered_device.hpp,
// kernels/clustered_lighting.cu). Plan rows (docs/plans/FUSE_MASTER_PLAN.md, B5 gates):
//   "Clustered cull + deferred shade runs in < 3ms for 1000 lights at 1080p — measured with CUDA events"
//   "Deferred shading + clustered lights (1000 lights): < 3ms"
//
// Scene: tests/b5_bench_scene.hpp (analytic room + 96 spheres ray-cast into the G-buffer, 1000 point lights),
// the default 16x9x24 cluster grid (256 lights per cluster).
//
//   --cpu      every build (ctest fuse_b5_clustered_device_cpu), 480 x 270: the device frame's kernel chain on the
//              CPU backends — clustered_light_grid CpuReference == CpuParallel bit for bit (0 / 2 / 4 workers);
//              shading over the fixed-capacity grid == shading over the compacted grid (cullLightsToClusterLists +
//              compactClusterLists + shadeDeferredFrame) bit for bit, stats included; ClusteredDeviceFrame refuses
//              cleanly without a device.
//   (default)  the RTX 3090 run (ctest fuse_b5_clustered_device; exit 77 without a CUDA device), 1920 x 1080:
//              device lists == the CPU pipeline (per-cluster counts on >= 99.5% of clusters) and radiance ==
//              CpuParallel within 1e-4 + 1e-4 |ref| on >= 99.5% of the pixels; then --iterations (default 100)
//              resident frames timed with CUDA events — bounds / bin / cull / grid / shade and the total
//              (budget 3 ms, reported; --enforce-budgets makes it fail).
//   --width / --height / --lights / --iterations override the device run.

#include "b5_bench_scene.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_device.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/clustered_shading.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace kernel = fuse::kernel;
namespace ck = fuse::renderer::clustered_kernel;
using fuse::math::Vec3;
using namespace fuse::renderer;

constexpr int kSkip = 77;
constexpr f64 kBudgetMs = 3.0;
int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct Options {
    bool cpu = false;
    bool enforce = false;
    u32 width = 1920;
    u32 height = 1080;
    u32 lights = 1000;
    u32 iterations = 100;
};

DeferredGBufferView viewOf(const b5bench::Frame& f) {
    DeferredGBufferView view{};
    view.width = f.width;
    view.height = f.height;
    view.deviceDepth = f.deviceDepth.data();
    view.normals = f.normals.data();
    view.albedo = f.albedo.data();
    return view;
}

/// The CPU clustered pipeline (every step on `backend`).
struct CpuPipeline {
    std::vector<ClusterAABB> aabbs;
    ClusterCullLists lists;
    ClusterGridSoA compacted;
    std::vector<Vec3> radiance;
    DeferredShadeStats stats{};
};

CpuPipeline runCpuPipeline(const b5bench::Frame& frame, const ClusterDesc& desc,
                           const std::vector<PointLightInput>& lights, kernel::Backend backend) {
    CpuPipeline p;
    cluster_math::buildClusterAabbs(desc, frame.camera, p.aabbs, backend);
    cluster_math::cullLightsToClusterLists(desc, frame.camera, p.aabbs, lights, {}, p.lists, backend);
    cluster_math::compactClusterLists(p.lists, desc.clusterCount(), p.compacted, backend);
    p.stats = clustered_shading::shadeDeferredFrame(viewOf(frame), desc, frame.camera, p.compacted, lights, true,
                                                    p.radiance, backend);
    return p;
}

/// The light grid the device frame uses: clustered_light_grid over the fixed-capacity cull lists.
std::vector<ClusterGridEntry> fixedGrid(const ClusterCullLists& lists, u32 clusters, kernel::Backend backend) {
    std::vector<ClusterGridEntry> grid(clusters);
    ck::GridParams params{};
    params.capacity = lists.capacity;
    params.counts = {lists.counts.data(), clusters};
    params.out_grid = {grid.data(), clusters};
    kernel::launch(backend, ck::make_linear_launch(ck::kGridName, clusters), ck::GridKernel{}, params);
    return grid;
}

// ---------------------------------------------------------------------------------------------
// --cpu
// ---------------------------------------------------------------------------------------------

int runCpuMode() {
    constexpr u32 kW = 480;
    constexpr u32 kH = 270;
    const b5bench::Scene scene = b5bench::makeScene();
    const b5bench::Frame frame = b5bench::makeFrame(scene, kW, kH);
    const std::vector<PointLightInput> lights = b5bench::makeLights(1000u);
    const ClusterDesc desc{};
    const u32 clusters = desc.clusterCount();
    expectTrue(frame.surfacePixels > kW * kH / 2u, "benchmark frame: most pixels hit geometry");

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const CpuPipeline reference = runCpuPipeline(frame, desc, lights, kernel::Backend::CpuReference);
    expectTrue(reference.stats.shadedPixels == frame.surfacePixels, "every surface pixel shaded");
    u64 listed = 0u;
    for (u32 c = 0; c < clusters; ++c) {
        listed += reference.lists.counts[c];
    }
    std::printf("CPU pipeline %ux%u, %zu lights: %llu cluster-light pairs, %llu light evaluations\n", kW, kH,
                lights.size(), static_cast<unsigned long long>(listed),
                static_cast<unsigned long long>(reference.stats.lightEvaluations));
    expectTrue(listed > 0u && reference.stats.lightEvaluations > 0u, "lights reach the frame");

    // clustered_light_grid: CpuReference == CpuParallel.
    const std::vector<ClusterGridEntry> gridRef = fixedGrid(reference.lists, clusters, kernel::Backend::CpuReference);
    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const std::vector<ClusterGridEntry> gridPar = fixedGrid(reference.lists, clusters, kernel::Backend::CpuParallel);
        char message[160];
        std::snprintf(message, sizeof(message), "clustered_light_grid CpuReference == CpuParallel (%u workers)", workers);
        expectTrue(kernel::compare_bitwise(std::span<const ClusterGridEntry>(gridRef),
                                           std::span<const ClusterGridEntry>(gridPar))
                       .ok,
                   message);
    }
    bool entries = true;
    for (u32 c = 0; c < clusters; ++c) {
        entries = entries && gridRef[c].offset == c * reference.lists.capacity && gridRef[c].count == reference.lists.counts[c];
    }
    expectTrue(entries, "clustered_light_grid entry = {cluster x capacity, count}");
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(ck::kGridName, stats) && stats.launches == 4u &&
                   stats.items == stats.launches * clusters,
               "clustered_light_grid records its stats (one item per cluster, 4 launches)");

    // Shading over the fixed-capacity grid (the device frame's layout) == shading over the compacted grid.
    ClusterGridSoA fixed{};
    fixed.grid = gridRef;
    fixed.lightList = reference.lists.slots;
    for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
        std::vector<Vec3> radiance;
        const DeferredShadeStats s = clustered_shading::shadeDeferredFrame(viewOf(frame), desc, frame.camera, fixed,
                                                                           lights, true, radiance, backend);
        const kernel::ParityReport r =
            kernel::compare_bitwise(std::span<const Vec3>(reference.radiance), std::span<const Vec3>(radiance));
        expectTrue(r.ok && s.shadedPixels == reference.stats.shadedPixels &&
                       s.skippedPixels == reference.stats.skippedPixels &&
                       s.lightEvaluations == reference.stats.lightEvaluations,
                   backend == kernel::Backend::CpuReference
                       ? "fixed-capacity grid shade == compacted grid shade bit for bit (CpuReference, stats too)"
                       : "fixed-capacity grid shade == compacted grid shade bit for bit (CpuParallel, stats too)");
    }
    scheduler.shutdown();

    if (!ClusteredDeviceFrame::available()) {
        ClusteredDeviceFrame device;
        expectTrue(!device.upload(viewOf(frame), desc, frame.camera, lights) && !device.message().empty(),
                   "no CUDA device: ClusteredDeviceFrame::upload fails with a message");
        std::printf("ClusteredDeviceFrame::upload without a device: %s\n", device.message().c_str());
        expectTrue(!device.run() && !device.ok(), "no CUDA device: run() is refused");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ---------------------------------------------------------------------------------------------
// Device run
// ---------------------------------------------------------------------------------------------

struct Series {
    std::vector<f64> values;
    void add(f64 v) { values.push_back(v); }
    f64 mean() const {
        f64 sum = 0.0;
        for (f64 v : values) {
            sum += v;
        }
        return values.empty() ? 0.0 : sum / static_cast<f64>(values.size());
    }
    f64 median() const {
        if (values.empty()) {
            return 0.0;
        }
        std::vector<f64> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2u];
    }
    f64 lowest() const { return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end()); }
    f64 highest() const { return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end()); }
};

int runDeviceMode(const Options& opt) {
    if (!ClusteredDeviceFrame::available()) {
        std::printf("SKIP: no CUDA device (fuse_b5_clustered_device needs FUSE_BUILD_CUDA=ON and an NVIDIA GPU; "
                    "the CPU half runs as fuse_b5_clustered_device_cpu)\n");
        return kSkip;
    }
#if defined(FUSE_HAS_CUDA)
    cudaDeviceProp props{};
    int deviceIndex = 0;
    if (cudaGetDevice(&deviceIndex) == cudaSuccess && cudaGetDeviceProperties(&props, deviceIndex) == cudaSuccess) {
        std::printf("CUDA device %d: %s (sm_%d%d)\n", deviceIndex, props.name, props.major, props.minor);
    }
#endif
    const b5bench::Scene scene = b5bench::makeScene();
    const b5bench::Frame frame = b5bench::makeFrame(scene, opt.width, opt.height);
    const std::vector<PointLightInput> lights = b5bench::makeLights(opt.lights);
    const ClusterDesc desc{};
    const u32 clusters = desc.clusterCount();

    ClusteredDeviceFrame device;
    if (!device.upload(viewOf(frame), desc, frame.camera, lights)) {
        std::fprintf(stderr, "FAIL: upload: %s\n", device.message().c_str());
        return EXIT_FAILURE;
    }
    expectTrue(device.run(), "first device frame runs without a CUDA error");
    std::vector<Vec3> radiance;
    DeferredShadeStats stats{};
    ClusterCullLists lists{};
    expectTrue(device.download(radiance, stats, &lists), "device results read back");

    // Parity against the CPU pipeline (CpuParallel on the workers fuse::core::initialize started;
    // CpuParallel == CpuReference is gated by fuse_clustered_kernel_parity).
    const CpuPipeline cpu = runCpuPipeline(frame, desc, lights, kernel::Backend::CpuParallel);
    u32 countMismatch = 0u;
    for (u32 c = 0; c < clusters; ++c) {
        countMismatch += lists.counts[c] != cpu.lists.counts[c] ? 1u : 0u;
    }
    std::printf("device vs CPU: %u of %u clusters with a different light count; shaded %u / %u, skipped %u / %u\n",
                countMismatch, clusters, stats.shadedPixels, cpu.stats.shadedPixels, stats.skippedPixels,
                cpu.stats.skippedPixels);
    expectTrue(countMismatch * 200u <= clusters, "device cull lists == CPU lists on >= 99.5% of clusters");
    expectTrue(stats.shadedPixels == cpu.stats.shadedPixels && stats.skippedPixels == cpu.stats.skippedPixels,
               "device shade counters == CPU");
    const kernel::ParityReport r = kernel::compare_floats(std::span<const Vec3>(cpu.radiance),
                                                          std::span<const Vec3>(radiance), {1e-4, 1e-4});
    std::printf("device vs CPU radiance: %llu of %llu values outside 1e-4 + 1e-4|ref| (max abs %.3g, max rel %.3g)\n",
                static_cast<unsigned long long>(r.mismatches), static_cast<unsigned long long>(r.compared),
                r.max_abs_error, r.max_rel_error);
    expectTrue(r.launches_ok && r.mismatches * 200u <= r.compared,
               "device radiance == CPU radiance within tolerance on >= 99.5% of the values");

    Series bounds;
    Series bin;
    Series cull;
    Series grid;
    Series shade;
    Series total;
    for (u32 i = 0; i < 5u; ++i) {
        device.run(); // warm-up
    }
    for (u32 i = 0; i < opt.iterations; ++i) {
        if (!device.run()) {
            std::fprintf(stderr, "FAIL: frame %u: %s\n", i, device.message().c_str());
            ++g_failures;
            break;
        }
        const ClusteredDeviceTiming& t = device.lastTiming();
        bounds.add(t.bounds_ms);
        bin.add(t.bin_ms);
        cull.add(t.cull_ms);
        grid.add(t.grid_ms);
        shade.add(t.shade_ms);
        total.add(t.total_ms);
    }
    std::printf("MEASURE: clustered cull + deferred shade, %ux%u, %u point lights, %u clusters (CUDA events, "
                "resident, %zu frames; AABB build once: %.3f ms):\n",
                opt.width, opt.height, opt.lights, clusters, total.values.size(), static_cast<f64>(device.buildMs()));
    const struct {
        const char* name;
        const Series* s;
    } rows[] = {{ck::kBoundsName, &bounds}, {ck::kBinName, &bin},     {ck::kCullName, &cull},
                {ck::kGridName, &grid},     {ck::kShadeName, &shade}, {"total (cull + shade)", &total}};
    for (const auto& row : rows) {
        std::printf("  %-26s mean %7.3f  median %7.3f  min %7.3f  max %7.3f ms\n", row.name, row.s->mean(),
                    row.s->median(), row.s->lowest(), row.s->highest());
    }
    const bool within = total.median() < kBudgetMs;
    std::printf("  budget < %.1f ms (median): %s\n", kBudgetMs, within ? "WITHIN" : "OVER");
    if (opt.enforce && fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(within, "clustered cull + deferred shade within 3 ms (--enforce-budgets)");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool parseU32(const char* text, u32& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v == 0ul || v > 100000ul) {
        return false;
    }
    out = static_cast<u32>(v);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--cpu") == 0) {
            opt.cpu = true;
        } else if (std::strcmp(argv[i], "--enforce-budgets") == 0) {
            opt.enforce = true;
        } else if (hasValue && std::strcmp(argv[i], "--width") == 0 && parseU32(argv[i + 1], opt.width)) {
            ++i;
        } else if (hasValue && std::strcmp(argv[i], "--height") == 0 && parseU32(argv[i + 1], opt.height)) {
            ++i;
        } else if (hasValue && std::strcmp(argv[i], "--lights") == 0 && parseU32(argv[i + 1], opt.lights)) {
            ++i;
        } else if (hasValue && std::strcmp(argv[i], "--iterations") == 0 && parseU32(argv[i + 1], opt.iterations)) {
            ++i;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--cpu] [--enforce-budgets] [--width W] [--height H] [--lights N] "
                         "[--iterations N]\n",
                         argv[0]);
            return EXIT_FAILURE;
        }
    }
    fuse::core::initialize();
    const int rc = opt.cpu ? runCpuMode() : runDeviceMode(opt);
    fuse::core::shutdown();
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_failures != 0 || rc != EXIT_SUCCESS) {
        std::fprintf(stderr, "fuse_b5_clustered_device%s: %d failure(s)\n", opt.cpu ? " --cpu" : "", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b5_clustered_device%s: all checks passed\n", opt.cpu ? " --cpu" : "");
    return EXIT_SUCCESS;
}
