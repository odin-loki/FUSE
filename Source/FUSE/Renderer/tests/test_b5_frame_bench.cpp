// B5 per-pass frame benchmark (docs/plans/FUSE_MASTER_PLAN.md B5 "Full Frame Performance (RTX 3090, 1920x1080)"
// and B6 "GPU pass breakdown times match CUDA event measurements within 0.5ms"). One harness that runs every B5
// pass the tree has on a device path at the requested resolution (default 1920x1080) and prints per-pass GPU
// times, the frame total and a profiler cross-check. Nothing is asserted unless --enforce is given.
//
//   depth prepass           no device implementation in the tree (the G-buffer pass depth-tests in-pass): reported
//   G-buffer (1000 objects) Vulkan  B5.2 GBufferRasterPass, GpuProfiler timestamp zone          (b5_frame_bench_vk.cpp)
//   SDF ray march           CUDA    sdf_ray_march, 100 objects, 128 steps, CUDA events         (cuda/b5_frame_bench_cuda.cu)
//   DDGI probe update       CUDA    DdgiDeviceVolume: 2048 probes, 64 x 256 rays per frame, events (ddgi_device.hpp)
//   clustered + deferred    CUDA    ClusteredDeviceFrame: 1000 lights, bounds/bin/cull/grid/shade (clustered_device.hpp)
//   SDF shadows             CUDA    sdf_shadows over the same 100 objects, CUDA events
//   HBAO                    CUDA    screen_space_ao + screen_space_ao_blur, CUDA events
//   SSR                     CUDA    screen_space_reflections, CUDA events
//   TAA                     Vulkan  WP-4.1 TaauGpu at 1x ("taau.*" render-graph passes), timestamps
//   post                    Vulkan  WP-4.5 PostStackGpu: bloom + DoF + motion blur + ACES ("post.*"), timestamps
//
// Profiler cross-check (CUDA passes): each pass runs a second loop with synchronous launches; the engine profiler's
// pass time (the sum of its kernels' kernel::LaunchRecord durations — the span FUSE_PROFILE_SCOPE(launch.name)
// records, i.e. the GPU pass breakdown the profiler shows) is compared with the CUDA-event time of the same
// iterations: MATCH when within 0.5 ms. For the Vulkan passes the profiler's breakdown IS the timestamp query.
//
// Options: --width W --height H --iterations N --warmup N --draws N --lights N --no-cuda --no-vulkan
//          --require-cuda (exit 77 without a CUDA device: the RTX 3090 ctest) --enforce (budgets + cross-check fail
//          the run; outside instrumented runs). Exit 77 when neither a CUDA nor a Vulkan device is present.

#include "b5_frame_bench.hpp"

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_device.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/lighting/clustered_device.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace b5bench;
using fuse::u64;
namespace kernel = fuse::kernel;
namespace rr = fuse::renderer;

constexpr int kSkip = 77;
constexpr f64 kCrossCheckMs = 0.5;
constexpr f64 kFrameBudgetMs = 16.0;

f64 meanOf(const std::vector<f64>& v) {
    f64 sum = 0.0;
    for (f64 x : v) {
        sum += x;
    }
    return v.empty() ? 0.0 : sum / static_cast<f64>(v.size());
}

f64 medianOf(std::vector<f64> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2u];
}

f64 lowestOf(const std::vector<f64>& v) {
    return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
}

f64 highestOf(const std::vector<f64>& v) {
    return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
}

/// Mean duration (ms) of every launch of `name` since the last reset_kernel_stats (-1 when never launched).
f64 kernelMeanMs(const char* name) {
    kernel::KernelStats s{};
    if (!kernel::find_kernel_stats(name, s) || s.launches == 0u) {
        return -1.0;
    }
    return static_cast<f64>(s.total_ns) * 1e-6 / static_cast<f64>(s.launches);
}

PassResult makeRow(const char* key, const char* label, const char* path, f64 budget) {
    PassResult r;
    r.key = key;
    r.label = label;
    r.path = path;
    r.budgetMs = budget;
    return r;
}

// ---------------------------------------------------------------------------------------------
// DDGI (DdgiDeviceVolume) and clustered (ClusteredDeviceFrame): the library's resident device paths
// ---------------------------------------------------------------------------------------------

[[maybe_unused]] rr::DdgiCpuScene ddgiScene(const Scene& scene) {
    rr::DdgiCpuScene out;
    for (const Box& b : scene.boxes) {
        rr::DdgiCpuSurface surface{};
        surface.albedo = b.albedo;
        out.addBox(b.lo, b.hi, surface);
    }
    for (const Sphere& s : scene.spheres) { // spheres as their inscribed cubes (the probe tracer is box-only)
        rr::DdgiCpuSurface surface{};
        surface.albedo = s.albedo;
        const f32 h = s.radius * 0.57735f;
        out.addBox(s.center - Vec3{h, h, h}, s.center + Vec3{h, h, h}, surface);
    }
    out.sun_direction = Vec3{0.4f, 1.f, 0.3f}.normalized();
    out.sun_irradiance = {2.f, 1.9f, 1.8f};
    out.sky_radiance = {0.2f, 0.3f, 0.5f};
    return out;
}

[[maybe_unused]] void benchDdgi(const BenchConfig& cfg, const Scene& scene, PassResult& out) {
    rr::DDGIDesc desc{}; // 16x8x16 = 2048 probes, 256 rays, 64 per frame, 8x8 / 16x16 texels
    desc.grid_origin = {-18.f, 0.75f, -38.f};
    desc.probe_spacing = {2.4f, 1.5f, 3.f};
    const rr::DdgiCpuScene ddgi = ddgiScene(scene);
    rr::DdgiCpuVolume volume;
    rr::DdgiDeviceVolume device;
    if (!volume.init(desc) || !device.upload(volume)) {
        out.failed = true;
        out.note = "upload: " + device.message();
        return;
    }
    u32 frame = 0u;
    for (u32 i = 0; i < std::max(cfg.warmup, 32u); ++i) { // one rotation: every probe has history
        device.update(volume, ddgi, frame++);
    }
    std::vector<f64> upload;
    std::vector<f64> kernels;
    for (u32 i = 0; i < cfg.iterations; ++i) {
        const rr::DdgiCpuUpdateStats s = device.update(volume, ddgi, frame++);
        if (!device.ok() || s.probes_updated != 64u) {
            out.failed = true;
            out.note = "update: " + device.message();
            return;
        }
        out.ms.push_back(device.lastTiming().cycle_ms);
        upload.push_back(device.lastTiming().upload_ms);
        kernels.push_back(device.lastTiming().trace_ms + device.lastTiming().blend_ms);
    }
    device.setSynchronousLaunches(true);
    kernel::reset_kernel_stats();
    f64 events = 0.0;
    for (u32 i = 0; i < cfg.iterations; ++i) {
        device.update(volume, ddgi, frame++);
        events += device.lastTiming().trace_ms + device.lastTiming().blend_ms;
    }
    const f64 trace = kernelMeanMs(rr::ddgi_kernel::kTraceName);
    const f64 blend = kernelMeanMs(rr::ddgi_kernel::kName);
    if (trace >= 0.0 && blend >= 0.0 && cfg.iterations > 0u) {
        out.profilerMs = trace + blend;
        out.profilerEventMs = events / static_cast<f64>(cfg.iterations);
    }
    out.ran = true;
    char note[200];
    std::snprintf(note, sizeof(note), "update cycle; upload %.3f + trace/blend %.3f ms (2048 probes, 64 x 256 rays)",
                  meanOf(upload), meanOf(kernels));
    out.note = note;
}

[[maybe_unused]] void benchClustered(const BenchConfig& cfg, const Frame& frame, PassResult& out) {
    namespace ck = rr::clustered_kernel;
    const std::vector<rr::PointLightInput> lights = makeLights(cfg.lights);
    rr::DeferredGBufferView view{};
    view.width = frame.width;
    view.height = frame.height;
    view.deviceDepth = frame.deviceDepth.data();
    view.normals = frame.normals.data();
    view.albedo = frame.albedo.data();
    rr::ClusteredDeviceFrame device;
    if (!device.upload(view, rr::ClusterDesc{}, frame.camera, lights)) {
        out.failed = true;
        out.note = "upload: " + device.message();
        return;
    }
    for (u32 i = 0; i < cfg.warmup; ++i) {
        device.run();
    }
    f64 parts[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
    for (u32 i = 0; i < cfg.iterations; ++i) {
        if (!device.run()) {
            out.failed = true;
            out.note = "run: " + device.message();
            return;
        }
        const rr::ClusteredDeviceTiming& t = device.lastTiming();
        out.ms.push_back(t.total_ms);
        parts[0] += t.bounds_ms;
        parts[1] += t.bin_ms;
        parts[2] += t.cull_ms;
        parts[3] += t.grid_ms;
        parts[4] += t.shade_ms;
    }
    device.setSynchronousLaunches(true);
    kernel::reset_kernel_stats();
    f64 events = 0.0;
    for (u32 i = 0; i < cfg.iterations; ++i) {
        device.run();
        const rr::ClusteredDeviceTiming& t = device.lastTiming();
        events += t.bounds_ms + t.bin_ms + t.cull_ms + t.grid_ms + t.shade_ms;
    }
    f64 profiler = 0.0;
    bool all = true;
    for (const char* name : {ck::kBoundsName, ck::kBinName, ck::kCullName, ck::kGridName, ck::kShadeName}) {
        const f64 ms = kernelMeanMs(name);
        all = all && ms >= 0.0;
        profiler += ms;
    }
    if (all && cfg.iterations > 0u) {
        out.profilerMs = profiler;
        out.profilerEventMs = events / static_cast<f64>(cfg.iterations);
    }
    out.ran = true;
    const f64 inv = cfg.iterations > 0u ? 1.0 / static_cast<f64>(cfg.iterations) : 0.0;
    char note[240];
    std::snprintf(note, sizeof(note), "bounds %.3f, bin %.3f, cull %.3f, grid %.3f, shade %.3f ms (%u lights)",
                  parts[0] * inv, parts[1] * inv, parts[2] * inv, parts[3] * inv, parts[4] * inv, cfg.lights);
    out.note = note;
}

// ---------------------------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------------------------

struct Totals {
    f64 sumMs = 0.0;
    u32 measured = 0;
    u32 overBudget = 0;
    u32 crossChecked = 0;
    u32 crossMismatch = 0;
    u32 failed = 0;
};

Totals report(const BenchConfig& cfg, const std::vector<PassResult>& rows, const std::string& cudaName,
              const std::string& vkName) {
    Totals t;
    std::printf("\n=== B5 frame benchmark %ux%u: %u timed iterations after %u warm-up ===\n", cfg.width, cfg.height,
                cfg.iterations, cfg.warmup);
    std::printf("CUDA device:   %s\nVulkan device: %s\n", cudaName.c_str(), vkName.c_str());
    std::printf("Budgets are the plan's RTX 3090 targets at 1920x1080; verdicts are informational unless --enforce.\n\n");
    std::printf("%-34s %-18s %8s %8s %8s %8s %8s  %s\n", "pass", "measured with", "mean", "median", "min", "max",
                "budget", "verdict");
    for (const PassResult& r : rows) {
        if (!r.ran) {
            std::printf("%-34s %-18s %8s %8s %8s %8s %8.2f  %s: %s\n", r.label.c_str(), r.path.c_str(), "-", "-", "-",
                        "-", r.budgetMs, r.failed ? "FAILED" : "NOT MEASURED", r.note.c_str());
            t.failed += r.failed ? 1u : 0u;
            continue;
        }
        const f64 mean = meanOf(r.ms);
        const bool within = mean < r.budgetMs;
        t.sumMs += mean;
        ++t.measured;
        t.overBudget += within ? 0u : 1u;
        t.failed += r.failed ? 1u : 0u;
        std::printf("%-34s %-18s %8.3f %8.3f %8.3f %8.3f %8.2f  %s%s (%s)\n", r.label.c_str(), r.path.c_str(), mean,
                    medianOf(r.ms), lowestOf(r.ms), highestOf(r.ms), r.budgetMs, within ? "WITHIN" : "OVER",
                    r.failed ? ", CHECK FAILED" : "", r.note.c_str());
    }
    std::printf("%-34s %-18s %8.3f %8s %8s %8s %8.2f  %s (%u of %zu passes measured)\n", "TOTAL GPU frame (sum of means)",
                "", t.sumMs, "", "", "", kFrameBudgetMs, t.sumMs < kFrameBudgetMs ? "WITHIN" : "OVER", t.measured,
                rows.size());

    std::printf("\nProfiler GPU pass breakdown vs CUDA events (synchronous launches, same iterations; "
                "kernel::LaunchRecord == FUSE_PROFILE_SCOPE span):\n");
    std::printf("%-34s %12s %12s %10s  %s\n", "pass", "profiler ms", "events ms", "|diff| ms", "check");
    for (const PassResult& r : rows) {
        if (!r.ran || r.profilerMs < 0.0) {
            continue;
        }
        const f64 diff = std::fabs(r.profilerMs - r.profilerEventMs);
        const bool match = diff <= kCrossCheckMs;
        ++t.crossChecked;
        t.crossMismatch += match ? 0u : 1u;
        std::printf("%-34s %12.3f %12.3f %10.3f  %s\n", r.label.c_str(), r.profilerMs, r.profilerEventMs, diff,
                    match ? "MATCH (<= 0.5 ms)" : "MISMATCH (> 0.5 ms)");
    }
    if (t.crossChecked == 0u) {
        std::printf("  (no CUDA pass ran: nothing to cross-check)\n");
    }

    std::printf("\n");
    for (const PassResult& r : rows) {
        if (r.ran) {
            std::printf("MEASURE: %s %ux%u %s mean %.3f ms median %.3f ms max %.3f ms budget %.2f ms%s\n", r.key.c_str(),
                        cfg.width, cfg.height, r.path.c_str(), meanOf(r.ms), medianOf(r.ms), highestOf(r.ms), r.budgetMs,
                        r.profilerMs >= 0.0 ? " (profiler cross-checked)" : "");
        }
    }
    std::printf("MEASURE: total %ux%u %.3f ms over %u measured passes (budget %.1f ms)\n", cfg.width, cfg.height,
                t.sumMs, t.measured, kFrameBudgetMs);
    return t;
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
    BenchConfig cfg;
    bool requireCuda = false;
    bool enforce = false;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        const char* a = argv[i];
        if (std::strcmp(a, "--no-cuda") == 0) {
            cfg.cuda = false;
        } else if (std::strcmp(a, "--no-vulkan") == 0) {
            cfg.vulkan = false;
        } else if (std::strcmp(a, "--require-cuda") == 0) {
            requireCuda = true;
        } else if (std::strcmp(a, "--enforce") == 0) {
            enforce = true;
        } else if (hasValue && std::strcmp(a, "--width") == 0 && parseU32(argv[i + 1], cfg.width)) {
            ++i;
        } else if (hasValue && std::strcmp(a, "--height") == 0 && parseU32(argv[i + 1], cfg.height)) {
            ++i;
        } else if (hasValue && std::strcmp(a, "--iterations") == 0 && parseU32(argv[i + 1], cfg.iterations)) {
            ++i;
        } else if (hasValue && std::strcmp(a, "--warmup") == 0 && parseU32(argv[i + 1], cfg.warmup)) {
            ++i;
        } else if (hasValue && std::strcmp(a, "--draws") == 0 && parseU32(argv[i + 1], cfg.draws)) {
            ++i;
        } else if (hasValue && std::strcmp(a, "--lights") == 0 && parseU32(argv[i + 1], cfg.lights)) {
            ++i;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--width W] [--height H] [--iterations N] [--warmup N] [--draws N] [--lights N] "
                         "[--no-cuda] [--no-vulkan] [--require-cuda] [--enforce]\n",
                         argv[0]);
            return EXIT_FAILURE;
        }
    }

    fuse::core::initialize();
    const bool cudaAvailable = cfg.cuda && kernel::backend_available(kernel::Backend::Cuda);
    if (requireCuda && !cudaAvailable) {
        std::printf("SKIP: no CUDA device (fuse_b5_frame_bench --require-cuda is the RTX 3090 run: FUSE_BUILD_CUDA=ON "
                    "and an NVIDIA GPU)\n");
        fuse::core::shutdown();
        return kSkip;
    }

    const Scene scene = makeScene();
    const Frame frame = makeFrame(scene, cfg.width, cfg.height);
    const std::vector<fuse::compute::SdfObject> objects = sdfObjects(scene);

    std::vector<PassResult> rows;
    rows.push_back(makeRow("depth_prepass", "Depth prepass", "-", 0.5));
    rows.push_back(makeRow("gbuffer", "G-buffer pass", "Vulkan timestamps", 2.0));
    rows.push_back(makeRow("sdf_ray_march", "SDF ray march (100 obj, 128 st)", "CUDA events", 3.0));
    rows.push_back(makeRow("ddgi_probe_update", "DDGI probe update (64 x 256)", "CUDA events", 2.0));
    rows.push_back(makeRow("clustered_deferred", "Deferred + clustered lights", "CUDA events", 3.0));
    rows.push_back(makeRow("sdf_shadows", "SDF shadows", "CUDA events", 2.0));
    rows.push_back(makeRow("screen_space_ao", "HBAO (+ blur)", "CUDA events", 1.0));
    rows.push_back(makeRow("screen_space_reflections", "SSR", "CUDA events", 1.5));
    rows.push_back(makeRow("taa", "TAA (TAAU 1x)", "Vulkan timestamps", 0.5));
    rows.push_back(makeRow("post", "Bloom + DoF + MB + tonemap", "Vulkan timestamps", 1.0));
    PassResult& depthPrepass = rows[0];
    PassResult& gbuffer = rows[1];
    PassResult& march = rows[2];
    PassResult& ddgi = rows[3];
    PassResult& clustered = rows[4];
    PassResult& shadows = rows[5];
    PassResult& hbao = rows[6];
    PassResult& ssr = rows[7];
    PassResult& taa = rows[8];
    PassResult& post = rows[9];
    depthPrepass.note = "no depth-only pass on a device path in the tree (the G-buffer raster pass depth-tests in-pass)";
    char drawsLabel[64];
    std::snprintf(drawsLabel, sizeof(drawsLabel), "G-buffer pass (%u objects)", cfg.draws);
    gbuffer.label = drawsLabel;
    char lightsLabel[64];
    std::snprintf(lightsLabel, sizeof(lightsLabel), "Deferred + clustered (%u lights)", cfg.lights);
    clustered.label = lightsLabel;

    std::string cudaName = cfg.cuda ? "none (no CUDA device / CUDA backend not built)" : "disabled (--no-cuda)";
    if (cudaAvailable) {
#if defined(FUSE_B5_FRAME_BENCH_CUDA)
        cudaName = cudaDeviceDescription();
        CudaPassInputs in{};
        in.frame = &frame;
        in.objects = &objects;
        in.sunDirection = Vec3{0.4f, 1.f, 0.3f}.normalized();
        const fuse::math::Mat4 proj = fuse::math::perspective(kFovYRadians * 180.f / 3.14159265f,
                                                              static_cast<f32>(cfg.width) / static_cast<f32>(cfg.height),
                                                              kNearPlane, kFarPlane);
        for (u32 k = 0; k < 16u; ++k) {
            in.proj[k] = proj.data[k];
        }
        std::printf("running CUDA passes on %s ...\n", cudaName.c_str());
        cudaBenchRayMarch(in, cfg, march);
        benchDdgi(cfg, scene, ddgi);
        benchClustered(cfg, frame, clustered);
        cudaBenchSdfShadows(in, cfg, shadows);
        cudaBenchHbao(in, cfg, hbao);
        cudaBenchSsr(in, cfg, ssr);
#else
        cudaName = "CUDA runtime present, but this build has no CUDA passes (FUSE_BUILD_CUDA=OFF)";
        (void)objects;
#endif
    }
    for (PassResult* r : {&march, &ddgi, &clustered, &shadows, &hbao, &ssr}) {
        if (!r->ran && !r->failed && r->note.empty()) {
            r->note = cudaAvailable ? "CUDA passes not built" : "no CUDA device";
        }
    }

    std::string vkName = cfg.vulkan ? "none" : "disabled (--no-vulkan)";
    bool vulkanDevice = false;
    if (cfg.vulkan) {
        std::string name;
        std::printf("running Vulkan passes ...\n");
        vulkanDevice = vulkanBench(cfg, frame, gbuffer, taa, post, name);
        if (!name.empty()) {
            vkName = name;
        }
    } else {
        for (PassResult* r : {&gbuffer, &taa, &post}) {
            r->note = "--no-vulkan";
        }
    }

    if (!cudaAvailable && !vulkanDevice) {
        std::printf("SKIP: no CUDA and no Vulkan device (nothing to time)\n");
        fuse::core::shutdown();
        return kSkip;
    }

    const Totals totals = report(cfg, rows, cudaName, vkName);
    fuse::core::shutdown();

    int rc = EXIT_SUCCESS;
    if (totals.failed != 0u) {
        std::fprintf(stderr, "FAIL: %u pass(es) failed to run or produced invalid output\n", totals.failed);
        rc = EXIT_FAILURE;
    }
    if (enforce && fuse::core::timingBudgetsEnforcedNoted()) {
        if (totals.overBudget != 0u || totals.sumMs >= kFrameBudgetMs) {
            std::fprintf(stderr, "FAIL (--enforce): %u pass(es) over budget, total %.3f ms\n", totals.overBudget,
                         totals.sumMs);
            rc = EXIT_FAILURE;
        }
        if (totals.crossMismatch != 0u) {
            std::fprintf(stderr, "FAIL (--enforce): %u profiler / CUDA-event mismatch(es) > 0.5 ms\n",
                         totals.crossMismatch);
            rc = EXIT_FAILURE;
        }
    }
    std::printf("fuse_b5_frame_bench: %s\n", rc == EXIT_SUCCESS ? "done" : "FAILED");
    return rc;
}
