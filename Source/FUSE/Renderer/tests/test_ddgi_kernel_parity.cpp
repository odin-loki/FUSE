// Gate for the single-source DDGI probe update (fuse/renderer/gi/ddgi_probe_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical irradiance / distance atlases,
//     update counts and stats over rolling updates (multi-bounce on and off), explicit lists with
//     out-of-range and duplicate probes, and a probe-count load scale. The trace grid (96 rays) has
//     partial 64-ray workgroups; the blend strides 36 / 100 texels over 64 threads.
//   - Launches record "ddgi_probe_trace" / "ddgi_probe_update" stats (items, workgroups, backend).
//   - Cuda / Auto / VulkanCompute requests without a device fall back to CpuParallel with the same result.

#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
namespace kernel = fuse::kernel;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

DdgiCpuScene makeRoom(f32 sun) {
    DdgiCpuScene scene;
    DdgiCpuSurface grey{};
    grey.albedo = {0.7f, 0.7f, 0.7f};
    DdgiCpuSurface red{};
    red.albedo = {0.8f, 0.1f, 0.1f};
    DdgiCpuSurface lamp{};
    lamp.emissive = {3.f, 2.f, 1.f};
    scene.addBox({-1.f, -1.f, -1.f}, {9.f, 0.f, 9.f}, grey);
    scene.addBox({-1.f, 0.f, -1.f}, {0.f, 6.f, 9.f}, red);
    scene.addBox({8.f, 0.f, -1.f}, {9.f, 6.f, 9.f}, grey);
    scene.addBox({0.f, 0.f, -1.f}, {8.f, 6.f, 0.f}, grey);
    scene.addBox({3.f, 1.f, 3.f}, {5.f, 3.f, 5.f}, lamp);   // probes inside it hit backfaces
    scene.addBox({2.f, 4.f, 6.f}, {7.f, 4.5f, 7.f}, grey);
    scene.sun_direction = Vec3{0.3f, 0.9f, 0.2f}.normalized();
    scene.sun_irradiance = {sun, sun * 0.9f, sun * 0.8f};
    scene.sky_radiance = {0.2f, 0.3f, 0.5f};
    return scene;
}

DDGIDesc makeDesc() {
    DDGIDesc desc{};
    desc.grid_origin = {0.5f, 0.5f, 0.5f};
    desc.probe_spacing = {1.5f, 1.4f, 1.6f};
    desc.grid_dims = {5u, 4u, 6u};
    desc.probes_per_frame = 17u;
    desc.rays_per_probe = 96u; // not a multiple of the 64-ray trace workgroup
    desc.irradiance_res = 6u;
    desc.depth_res = 10u;
    return desc;
}

struct Run {
    std::vector<Vec3> irradiance;
    std::vector<Vec2> distance;
    std::vector<u32> updateCounts;
    std::vector<DdgiCpuUpdateStats> stats;
};

/// Rolling updates with a sun change (probe-level change detection), then explicit lists with an
/// out-of-range probe and duplicates, on one backend.
Run simulate(kernel::Backend backend, bool multiBounce, bool duplicates) {
    DdgiCpuConfig config{};
    config.multi_bounce = multiBounce;
    config.probe_change_hysteresis = 0.3f;
    DdgiCpuVolume volume;
    expectTrue(volume.init(makeDesc(), config), "volume init");
    volume.setBackend(backend);
    Run run;
    for (u32 frame = 0; frame < 14u; ++frame) {
        run.stats.push_back(volume.update(makeRoom(frame < 7u ? 2.f : 6.f), frame));
    }
    const u32 withDuplicates[] = {3u, 7u, 3u, 500u, 11u, 7u, 0u};
    const u32 unique[] = {1u, 2u, 5u, 119u, 60u, 9999u};
    run.stats.push_back(duplicates ? volume.updateProbes(makeRoom(1.f), withDuplicates, 7u, 99u)
                                   : volume.updateProbes(makeRoom(1.f), unique, 6u, 99u));
    run.irradiance = volume.irradianceAtlas();
    run.distance = volume.distanceAtlas();
    for (u32 p = 0; p < volume.probeCount(); ++p) {
        run.updateCounts.push_back(volume.probeUpdateCount(p));
    }
    return run;
}

kernel::ParityReport compareRuns(const Run& a, const Run& b) {
    kernel::ParityReport report =
        kernel::compare_bitwise(std::span<const Vec3>(a.irradiance), std::span<const Vec3>(b.irradiance));
    report.merge(kernel::compare_bitwise(std::span<const Vec2>(a.distance), std::span<const Vec2>(b.distance)));
    report.merge(kernel::compare_bitwise(std::span<const u32>(a.updateCounts), std::span<const u32>(b.updateCounts)));
    report.merge(kernel::compare_bitwise(std::span<const DdgiCpuUpdateStats>(a.stats),
                                         std::span<const DdgiCpuUpdateStats>(b.stats)));
    return report;
}

void testBackendParity() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (bool multiBounce : {true, false}) {
        for (bool duplicates : {false, true}) {
            scheduler.shutdown();
            const Run reference = simulate(kernel::Backend::CpuReference, multiBounce, duplicates);
            u32 fast = 0u;
            for (const DdgiCpuUpdateStats& s : reference.stats) {
                fast += s.fast_response_texels;
            }
            expectTrue(fast > 0u, "reference run exercises change detection (fast-response texels)");
            expectTrue(reference.stats.back().probes_updated == (duplicates ? 6u : 5u),
                       "explicit list skips out-of-range probes, counts duplicates");
            for (u32 workers : {0u, 2u, 4u}) {
                scheduler.shutdown();
                scheduler.initialize(workers);
                const Run parallel = simulate(kernel::Backend::CpuParallel, multiBounce, duplicates);
                const kernel::ParityReport report = compareRuns(reference, parallel);
                char label[160];
                std::snprintf(label, sizeof(label),
                              "DDGI CpuReference == CpuParallel bit-exact (%u workers, multi-bounce %d, duplicates %d, "
                              "%llu mismatches)",
                              workers, multiBounce ? 1 : 0, duplicates ? 1 : 0,
                              static_cast<unsigned long long>(report.mismatches));
                expectTrue(report.ok && report.compared > 0u, label);
            }
        }
    }
    scheduler.shutdown();
}

void testLoadScale() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::LoadScale scale = kernel::load_scale();
    scale.probes = 0.5f;
    const kernel::ScopedLoadScale scoped(scale);
    const u32 expected = kernel::scaled_count(makeDesc().probes_per_frame, 0.5f);
    DdgiCpuVolume a;
    DdgiCpuVolume b;
    a.init(makeDesc());
    b.init(makeDesc());
    a.setBackend(kernel::Backend::CpuReference);
    const DdgiCpuUpdateStats sa = a.update(makeRoom(2.f), 3u);
    const DdgiCpuUpdateStats sb = b.update(makeRoom(2.f), 3u);
    expectTrue(sa.probes_updated == expected && sb.probes_updated == expected,
               "LoadScale::probes scales the rolling per-frame probe budget");
    expectTrue(kernel::compare_bitwise(std::span<const Vec3>(a.irradianceAtlas()),
                                       std::span<const Vec3>(b.irradianceAtlas()))
                   .ok,
               "load-scaled update: CpuReference == CpuParallel");
    scheduler.shutdown();
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    const DDGIDesc desc = makeDesc();
    DdgiCpuVolume reference;
    reference.init(desc);
    reference.setBackend(kernel::Backend::CpuReference);
    const u32 ids[] = {4u, 8u, 15u, 16u, 23u, 42u};
    reference.updateProbes(makeRoom(2.f), ids, 6u, 5u);

    kernel::KernelStats trace{};
    kernel::KernelStats blend{};
    expectTrue(kernel::find_kernel_stats(ddgi_kernel::kTraceName, trace) && trace.launches == 1u &&
                   trace.items == 6u * desc.rays_per_probe && trace.workgroups == 2u * 6u &&
                   trace.last_backend == kernel::Backend::CpuReference,
               "ddgi_probe_trace stats: rays x probes items, 64-ray workgroups");
    expectTrue(kernel::find_kernel_stats(ddgi_kernel::kName, blend) && blend.launches == 1u &&
                   blend.items == 6u * ddgi_kernel::kBlendThreads && blend.workgroups == 6u &&
                   blend.last_backend == kernel::Backend::CpuReference,
               "ddgi_probe_update stats: one workgroup per probe");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            DdgiCpuVolume volume;
            volume.init(desc);
            volume.setBackend(gpu);
            volume.updateProbes(makeRoom(2.f), ids, 6u, 5u);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel &&
                           last.name != nullptr && std::string_view(last.name) == ddgi_kernel::kName,
                       "GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(kernel::compare_bitwise(std::span<const Vec3>(reference.irradianceAtlas()),
                                               std::span<const Vec3>(volume.irradianceAtlas()))
                               .ok &&
                           kernel::compare_bitwise(std::span<const Vec2>(reference.distanceAtlas()),
                                                   std::span<const Vec2>(volume.distanceAtlas()))
                               .ok,
                       "fallback atlases == CpuReference atlases");
        }
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testLoadScale();
    testStatsAndFallback();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d DDGI kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("DDGI kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
