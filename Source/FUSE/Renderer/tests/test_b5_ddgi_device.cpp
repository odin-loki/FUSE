// B5.6 DDGI device gate (fuse/renderer/gi/ddgi_device.hpp, kernels/ddgi_probe_update.cu). Plan rows
// (docs/plans/FUSE_MASTER_PLAN.md, B5 gates):
//   "DDGI probes initialise and first update completes without CUDA error"
//   "2048 probes update 64 per frame at < 2ms per update cycle"
//   "DDGI probe update (64 probes, 256 rays): < 2ms"
//
//   --cpu      every build (ctest fuse_b5_ddgi_device_cpu): the two timed workloads on the CPU backends —
//              CpuReference == CpuParallel bit for bit at 0 / 2 / 4 workers for a 64-probe volume (4x4x4,
//              256 rays, every probe updated each frame, 3 updates) and for the 2048-probe volume (16x8x16,
//              64 probes per frame, 6 rolling frames); the device mirror refuses cleanly without a device.
//   (default)  the RTX 3090 run (ctest fuse_b5_ddgi_device; exit 77 without a CUDA device):
//              1. init + upload + first update without a CUDA error (64 probes x 256 rays; update counts 1 / 0);
//              2. 32 rolling frames (one rotation of 2048 probes): resident device atlases == the one-shot
//                 DdgiCpuVolume Backend::Cuda path bit for bit (same kernels, same device), and == CpuReference
//                 within |d| <= 2e-3 + 2e-3 |ref| on all but 0.5% of the values; update counts exact;
//              3. CUDA-event timing on resident atlases: 64 probes x 256 rays (64-probe volume) and the
//                 2048-probe volume at 64 probes per frame over 64 frames — upload / trace / blend / cycle.
//              The 2 ms budgets are reported; --enforce-budgets makes them fail (outside instrumented runs).

#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_device.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
namespace kernel = fuse::kernel;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

constexpr int kSkip = 77;
constexpr f64 kBudgetMs = 2.0;
int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// The B5 DDGI room (test_b5_ddgi_gates' big room) plus an emissive panel, so the 2048 probes see sun, sky,
/// bounce and emission.
DdgiCpuScene makeRoom() {
    DdgiCpuScene scene;
    DdgiCpuSurface white{};
    white.albedo = {0.8f, 0.8f, 0.8f};
    DdgiCpuSurface red{};
    red.albedo = {0.8f, 0.1f, 0.1f};
    DdgiCpuSurface green{};
    green.albedo = {0.1f, 0.8f, 0.1f};
    DdgiCpuSurface blue{};
    blue.albedo = {0.1f, 0.2f, 0.8f};
    DdgiCpuSurface lamp{};
    lamp.albedo = {0.2f, 0.2f, 0.2f};
    lamp.emissive = {4.f, 3.f, 2.f};
    scene.addBox({-1.f, -0.5f, -1.f}, {33.f, 0.f, 33.f}, white);
    scene.addBox({-0.5f, 0.f, -1.f}, {0.f, 16.f, 33.f}, red);
    scene.addBox({32.f, 0.f, -1.f}, {32.5f, 16.f, 33.f}, green);
    scene.addBox({-1.f, 0.f, -0.5f}, {33.f, 16.f, 0.f}, white);
    scene.addBox({10.f, 0.f, 10.f}, {14.f, 8.f, 14.f}, white);
    scene.addBox({20.f, 0.f, 18.f}, {24.f, 5.f, 26.f}, blue);
    scene.addBox({4.f, 6.f, 26.f}, {9.f, 6.5f, 30.f}, lamp);
    scene.sun_direction = Vec3{0.3f, 1.f, 0.5f}.normalized();
    scene.sun_irradiance = {1.f, 0.95f, 0.9f};
    scene.sky_radiance = {0.05f, 0.06f, 0.08f};
    return scene;
}

/// The 2048-probe volume of the plan row (default DDGIDesc: 16x8x16, 256 rays, 64 per frame, 8 / 16 texels).
DDGIDesc desc2048() {
    DDGIDesc desc{};
    desc.grid_origin = {1.f, 1.f, 1.f};
    return desc;
}

/// 64 probes x 256 rays: every probe of a 4x4x4 volume updates each frame.
DDGIDesc desc64() {
    DDGIDesc desc = desc2048();
    desc.grid_origin = {6.f, 1.f, 6.f};
    desc.grid_dims = {4u, 4u, 4u};
    desc.probes_per_frame = 64u;
    return desc;
}

struct Snapshot {
    std::vector<Vec3> irradiance;
    std::vector<Vec2> distance;
    std::vector<u32> counts;
    std::vector<DdgiCpuUpdateStats> stats;
};

Snapshot snapshot(const DdgiCpuVolume& volume) {
    Snapshot s;
    s.irradiance = volume.irradianceAtlas();
    s.distance = volume.distanceAtlas();
    for (u32 p = 0; p < volume.probeCount(); ++p) {
        s.counts.push_back(volume.probeUpdateCount(p));
    }
    return s;
}

kernel::ParityReport compareExact(const Snapshot& a, const Snapshot& b) {
    kernel::ParityReport r =
        kernel::compare_bitwise(std::span<const Vec3>(a.irradiance), std::span<const Vec3>(b.irradiance));
    r.merge(kernel::compare_bitwise(std::span<const Vec2>(a.distance), std::span<const Vec2>(b.distance)));
    r.merge(kernel::compare_bitwise(std::span<const u32>(a.counts), std::span<const u32>(b.counts)));
    return r;
}

// ---------------------------------------------------------------------------------------------
// --cpu: CpuReference == CpuParallel on the timed workloads
// ---------------------------------------------------------------------------------------------

Snapshot runCpu(kernel::Backend backend, const DDGIDesc& desc, u32 frames) {
    DdgiCpuVolume volume;
    expectTrue(volume.init(desc), "volume init");
    volume.setBackend(backend);
    const DdgiCpuScene scene = makeRoom();
    Snapshot s;
    std::vector<DdgiCpuUpdateStats> stats;
    for (u32 f = 0; f < frames; ++f) {
        stats.push_back(volume.update(scene, f));
    }
    s = snapshot(volume);
    s.stats = stats;
    return s;
}

void cpuParity(const char* label, const DDGIDesc& desc, u32 frames) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const Snapshot reference = runCpu(kernel::Backend::CpuReference, desc, frames);
    u32 blended = 0u;
    for (const DdgiCpuUpdateStats& s : reference.stats) {
        blended += s.probes_updated;
        expectTrue(s.rays_traced == s.probes_updated * desc.rays_per_probe, "rays traced = probes x rays_per_probe");
    }
    expectTrue(blended == frames * std::min(desc.probes_per_frame, ddgi_util::probeCount(desc)),
               "every frame blends probes_per_frame probes");
    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const Snapshot parallel = runCpu(kernel::Backend::CpuParallel, desc, frames);
        kernel::ParityReport report = compareExact(reference, parallel);
        report.merge(kernel::compare_bitwise(std::span<const DdgiCpuUpdateStats>(reference.stats),
                                             std::span<const DdgiCpuUpdateStats>(parallel.stats)));
        char message[200];
        std::snprintf(message, sizeof(message),
                      "%s: CpuReference == CpuParallel bit for bit (%u workers, %llu of %llu values differ)", label,
                      workers, static_cast<unsigned long long>(report.mismatches),
                      static_cast<unsigned long long>(report.compared));
        expectTrue(report.ok && report.compared > 0u, message);
    }
    scheduler.shutdown();
    std::printf("%s: CpuReference == CpuParallel (0 / 2 / 4 workers) over %u updates\n", label, frames);
}

int runCpuMode() {
    cpuParity("64 probes x 256 rays (4x4x4, all probes per frame)", desc64(), 3u);
    cpuParity("2048 probes, 64 per frame (16x8x16)", desc2048(), 6u);

    if (!DdgiDeviceVolume::available()) {
        DdgiCpuVolume volume;
        volume.init(desc64());
        DdgiDeviceVolume device;
        expectTrue(!device.upload(volume) && !device.ok() && !device.message().empty(),
                   "no CUDA device: DdgiDeviceVolume::upload fails with a message");
        std::printf("DdgiDeviceVolume::upload without a device: %s\n", device.message().c_str());
        const DdgiCpuUpdateStats s = device.update(volume, makeRoom(), 0u);
        expectTrue(!device.ok() && s.probes_updated == 0u, "no CUDA device: a device update is refused");
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

void printDevice() {
#if defined(FUSE_HAS_CUDA)
    int device = 0;
    cudaDeviceProp props{};
    if (cudaGetDevice(&device) == cudaSuccess && cudaGetDeviceProperties(&props, device) == cudaSuccess) {
        std::printf("CUDA device %d: %s (sm_%d%d, %d SMs)\n", device, props.name, props.major, props.minor,
                    props.multiProcessorCount);
    }
#endif
}

void deviceInitAndFirstUpdate() {
    const DdgiCpuScene scene = makeRoom();
    DdgiCpuVolume volume;
    expectTrue(volume.init(desc2048()), "2048-probe volume initialises");
    DdgiDeviceVolume device;
    const bool uploaded = device.upload(volume);
    if (!uploaded) {
        std::fprintf(stderr, "  upload: %s\n", device.message().c_str());
    }
    expectTrue(uploaded && device.resident(), "device atlases allocated and uploaded without a CUDA error");
    const DdgiCpuUpdateStats stats = device.update(volume, scene, 0u);
    if (!device.ok()) {
        std::fprintf(stderr, "  first update: %s\n", device.message().c_str());
    }
    expectTrue(device.ok(), "first device update (trace + blend) completes without a CUDA error");
    expectTrue(stats.probes_updated == 64u && stats.rays_traced == 64u * 256u, "first update: 64 probes x 256 rays");
    expectTrue(device.download(volume), "device atlases read back");
    expectTrue(volume.probeUpdateCount(0u) == 1u && volume.probeUpdateCount(63u) == 1u &&
                   volume.probeUpdateCount(64u) == 0u,
               "first update touched exactly the scheduled probes");
    bool finite = true;
    for (const Vec3& t : volume.irradianceAtlas()) {
        finite = finite && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) && t.x >= 0.f &&
                 t.y >= 0.f && t.z >= 0.f;
    }
    for (const Vec2& m : volume.distanceAtlas()) {
        finite = finite && std::isfinite(m.x) && std::isfinite(m.y) && m.x >= 0.f;
    }
    expectTrue(finite, "device atlases finite and non-negative");
    std::printf("init + first update: ok (%s), cycle %.3f ms (first launch includes module load)\n",
                device.ok() ? "no CUDA error" : device.message().c_str(), device.lastTiming().cycle_ms);
}

void deviceParity() {
    const DdgiCpuScene scene = makeRoom();
    const DDGIDesc desc = desc2048();
    DdgiCpuVolume reference;
    DdgiCpuVolume oneShot;
    DdgiCpuVolume mirror;
    expectTrue(reference.init(desc) && oneShot.init(desc) && mirror.init(desc), "parity volumes initialise");
    reference.setBackend(kernel::Backend::CpuReference);
    oneShot.setBackend(kernel::Backend::Cuda);
    DdgiDeviceVolume device;
    expectTrue(device.upload(mirror), "parity: resident upload");
    constexpr u32 kFrames = 32u; // one rotation: every probe blended once
    bool statsMatch = true;
    bool oneShotOnDevice = true;
    for (u32 f = 0; f < kFrames; ++f) {
        const DdgiCpuUpdateStats r = reference.update(scene, f);
        const DdgiCpuUpdateStats o = oneShot.update(scene, f);
        oneShotOnDevice = oneShotOnDevice && kernel::last_launch().backend == kernel::Backend::Cuda;
        const DdgiCpuUpdateStats d = device.update(mirror, scene, f);
        statsMatch = statsMatch && device.ok() && d.probes_updated == r.probes_updated &&
                     d.rays_traced == r.rays_traced && o.probes_updated == r.probes_updated &&
                     d.fast_response_texels == o.fast_response_texels;
    }
    expectTrue(oneShotOnDevice, "DdgiCpuVolume Backend::Cuda ran on the device (kernel stats backend = Cuda)");
    expectTrue(statsMatch, "device stats == CPU stats (probes, rays); resident == one-shot fast-response texels");
    expectTrue(device.download(mirror), "parity: resident atlases read back");

    const Snapshot ref = snapshot(reference);
    const Snapshot one = snapshot(oneShot);
    const Snapshot res = snapshot(mirror);
    const kernel::ParityReport same = compareExact(res, one);
    std::printf("resident vs one-shot device path: %llu of %llu values differ\n",
                static_cast<unsigned long long>(same.mismatches), static_cast<unsigned long long>(same.compared));
    expectTrue(same.ok, "resident device path == one-shot device path bit for bit");

    const kernel::Tolerance tol{2e-3, 2e-3};
    kernel::ParityReport close =
        kernel::compare_floats(std::span<const Vec3>(ref.irradiance), std::span<const Vec3>(res.irradiance), tol);
    close.merge(kernel::compare_floats(std::span<const Vec2>(ref.distance), std::span<const Vec2>(res.distance), tol));
    const kernel::ParityReport counts =
        kernel::compare_bitwise(std::span<const u32>(ref.counts), std::span<const u32>(res.counts));
    std::printf("device vs CpuReference after %u frames: %llu of %llu values outside |d| <= 2e-3 + 2e-3|ref| "
                "(max abs %.3g, max rel %.3g)\n",
                kFrames, static_cast<unsigned long long>(close.mismatches),
                static_cast<unsigned long long>(close.compared), close.max_abs_error, close.max_rel_error);
    expectTrue(close.launches_ok && close.mismatches * 200u <= close.compared,
               "device atlases == CpuReference within tolerance (<= 0.5% outliers: device ulps, grazing hits)");
    expectTrue(counts.ok, "device update counts == CpuReference");
}

struct TimingResult {
    Series upload;
    Series trace;
    Series blend;
    Series cycle;
};

TimingResult timeWorkload(const DDGIDesc& desc, u32 warmup, u32 frames) {
    const DdgiCpuScene scene = makeRoom();
    TimingResult t;
    DdgiCpuVolume volume;
    DdgiDeviceVolume device;
    if (!volume.init(desc) || !device.upload(volume)) {
        expectTrue(false, "timing volume uploads");
        return t;
    }
    for (u32 f = 0; f < warmup; ++f) {
        device.update(volume, scene, f);
    }
    for (u32 f = warmup; f < warmup + frames; ++f) {
        device.update(volume, scene, f);
        if (!device.ok()) {
            std::fprintf(stderr, "  frame %u: %s\n", f, device.message().c_str());
            expectTrue(false, "timed device update");
            return t;
        }
        const DdgiDeviceTiming& d = device.lastTiming();
        t.upload.add(d.upload_ms);
        t.trace.add(d.trace_ms);
        t.blend.add(d.blend_ms);
        t.cycle.add(d.cycle_ms);
    }
    return t;
}

bool report(const char* label, const TimingResult& t, bool enforce) {
    std::printf("MEASURE: %s (CUDA events, resident atlases, %zu updates):\n", label, t.cycle.values.size());
    const struct {
        const char* name;
        const Series* s;
    } rows[] = {{"upload (list + rays + boxes)", &t.upload},
                {"ddgi_probe_trace", &t.trace},
                {"ddgi_probe_update (blend)", &t.blend},
                {"update cycle", &t.cycle}};
    for (const auto& row : rows) {
        std::printf("  %-30s mean %7.3f  median %7.3f  min %7.3f  max %7.3f ms\n", row.name, row.s->mean(),
                    row.s->median(), row.s->lowest(), row.s->highest());
    }
    const bool within = t.cycle.median() < kBudgetMs;
    std::printf("  budget < %.1f ms per update cycle (median): %s\n", kBudgetMs, within ? "WITHIN" : "OVER");
    if (enforce && fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(within, "DDGI update cycle within the 2 ms budget (--enforce-budgets)");
    }
    return within;
}

int runDeviceMode(bool enforce) {
    if (!DdgiDeviceVolume::available()) {
        std::printf("SKIP: no CUDA device (fuse_b5_ddgi_device needs FUSE_BUILD_CUDA=ON and an NVIDIA GPU; "
                    "the CPU half runs as fuse_b5_ddgi_device_cpu)\n");
        return kSkip;
    }
    printDevice();
    deviceInitAndFirstUpdate();
    deviceParity();
    const TimingResult t64 = timeWorkload(desc64(), 8u, 128u);
    report("DDGI probe update, 64 probes x 256 rays (4x4x4 volume, every probe each frame)", t64, enforce);
    const TimingResult t2048 = timeWorkload(desc2048(), 32u, 64u);
    report("DDGI rolling update, 2048 probes at 64 per frame (16x8x16, 256 rays)", t2048, enforce);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    bool cpu = false;
    bool enforce = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cpu") == 0) {
            cpu = true;
        } else if (std::strcmp(argv[i], "--enforce-budgets") == 0) {
            enforce = true;
        } else {
            std::fprintf(stderr, "usage: %s [--cpu] [--enforce-budgets]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    fuse::core::initialize();
    const int rc = cpu ? runCpuMode() : runDeviceMode(enforce);
    fuse::core::shutdown();
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b5_ddgi_device%s: %d failure(s)\n", cpu ? " --cpu" : "", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b5_ddgi_device%s: all checks passed\n", cpu ? " --cpu" : "");
    return EXIT_SUCCESS;
}
