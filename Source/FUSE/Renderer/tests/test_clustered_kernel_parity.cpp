// Gate for the single-source clustered cull + deferred shade (fuse/renderer/lighting/clustered_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical cluster AABBs, per-cluster light
//     lists (counts, overflow), compacted light grids and shaded frames (clustered and brute force, with
//     identical stats) on grids with partial workgroups (15x9x23 clusters, 173x97 pixels in 8x8 tiles).
//     Light counts follow kernel::LoadScale::lights.
//   - Clustered shading equals looping every light (no cluster omits a light that reaches a pixel).
//   - Launches record clustered_cluster_build / clustered_light_bounds / clustered_light_bin / clustered_light_cull /
//     clustered_light_compact / deferred_shading stats; GPU requests without a device fall back.

#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/clustered_shading.hpp>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
using fuse::math::Vec3;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kWidth = 173;
constexpr u32 kHeight = 97;

ClusterDesc makeDesc(u32 maxLights) {
    ClusterDesc desc{};
    desc.tilesX = 15u;
    desc.tilesY = 9u;
    desc.slicesZ = 23u; // 3105 clusters: not a multiple of the 64-item workgroup
    desc.maxLightsPerCluster = maxLights;
    return desc;
}

ClusterCameraDesc makeCamera(bool reversedZ) {
    ClusterCameraDesc camera{};
    camera.position = {1.f, 2.f, 3.f};
    camera.forward = {0.2f, -0.1f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 120.f;
    camera.screenWidth = kWidth;
    camera.screenHeight = kHeight;
    camera.reversedZ = reversedZ;
    return camera;
}

struct Scene {
    std::vector<PointLightInput> points;
    std::vector<SpotLightInput> spots;
    std::vector<f32> depth;
    std::vector<Vec3> normals;
    std::vector<Vec3> albedo;
};

Scene makeScene(const ClusterCameraDesc& camera) {
    Scene scene;
    std::mt19937 rng(20260923u);
    std::uniform_real_distribution<f32> u01(0.f, 1.f);
    const u32 pointCount = kernel::scaled_count(600u, kernel::load_scale().lights);
    const u32 spotCount = kernel::scaled_count(40u, kernel::load_scale().lights);
    for (u32 i = 0; i < pointCount; ++i) {
        PointLightInput light{};
        light.position = {u01(rng) * 60.f - 30.f, u01(rng) * 20.f - 10.f, -u01(rng) * 80.f};
        light.radius = i % 97u == 0u ? 0.f : u01(rng) * 7.f; // some lights have no range
        light.color = {u01(rng), u01(rng), u01(rng)};
        light.intensity = 5.f * u01(rng);
        scene.points.push_back(light);
    }
    for (u32 i = 0; i < spotCount; ++i) {
        SpotLightInput spot{};
        spot.position = {u01(rng) * 40.f - 20.f, u01(rng) * 10.f, -u01(rng) * 50.f};
        spot.radius = u01(rng) * 8.f;
        scene.spots.push_back(spot);
    }
    for (u32 i = 0; i < kWidth * kHeight; ++i) {
        const f32 viewDepth = 0.05f + u01(rng) * 130.f; // some outside [near, far]
        scene.depth.push_back(i % 13u == 0u ? (camera.reversedZ ? 0.f : 1.f)
                                            : cluster_math::deviceDepthFromViewDepth(viewDepth, camera));
        scene.normals.push_back(Vec3{u01(rng) - 0.5f, u01(rng), u01(rng) - 0.5f}.normalized());
        scene.albedo.push_back({u01(rng), u01(rng), u01(rng)});
    }
    return scene;
}

struct Frame {
    std::vector<ClusterAABB> aabbs;
    ClusterCullLists lists;
    ClusterCullResult cull{};
    ClusterGridSoA grid;
    std::vector<Vec3> clustered;
    std::vector<Vec3> bruteForce;
    DeferredShadeStats clusteredStats{};
    DeferredShadeStats bruteStats{};
};

Frame render(kernel::Backend backend, const ClusterDesc& desc, const ClusterCameraDesc& camera, const Scene& scene) {
    Frame f;
    cluster_math::buildClusterAabbs(desc, camera, f.aabbs, backend);
    f.cull = cluster_math::cullLightsToClusterLists(desc, camera, f.aabbs, scene.points, scene.spots, f.lists, backend);
    cluster_math::compactClusterLists(f.lists, desc.clusterCount(), f.grid, backend);
    const DeferredGBufferView gbuffer{kWidth, kHeight, scene.depth.data(), scene.normals.data(), scene.albedo.data()};
    f.clusteredStats =
        clustered_shading::shadeDeferredFrame(gbuffer, desc, camera, f.grid, scene.points, true, f.clustered, backend);
    f.bruteStats =
        clustered_shading::shadeDeferredFrame(gbuffer, desc, camera, f.grid, scene.points, false, f.bruteForce, backend);
    return f;
}

kernel::ParityReport compareFrames(const Frame& a, const Frame& b) {
    kernel::ParityReport r =
        kernel::compare_bitwise(std::span<const ClusterAABB>(a.aabbs), std::span<const ClusterAABB>(b.aabbs));
    r.merge(kernel::compare_bitwise(std::span<const u32>(a.lists.counts), std::span<const u32>(b.lists.counts)));
    r.merge(kernel::compare_bitwise(std::span<const u32>(a.lists.dropped), std::span<const u32>(b.lists.dropped)));
    r.merge(kernel::compare_bitwise(std::span<const u32>(a.lists.sliceCounts), std::span<const u32>(b.lists.sliceCounts)));
    r.merge(kernel::compare_bitwise(std::span<const ClusterLightBounds>(a.lists.bounds),
                                    std::span<const ClusterLightBounds>(b.lists.bounds)));
    r.merge(kernel::compare_bitwise(std::span<const ClusterGridEntry>(a.grid.grid),
                                    std::span<const ClusterGridEntry>(b.grid.grid)));
    r.merge(kernel::compare_bitwise(std::span<const u32>(a.grid.lightList), std::span<const u32>(b.grid.lightList)));
    r.merge(kernel::compare_bitwise(std::span<const Vec3>(a.clustered), std::span<const Vec3>(b.clustered)));
    r.merge(kernel::compare_bitwise(std::span<const Vec3>(a.bruteForce), std::span<const Vec3>(b.bruteForce)));
    const DeferredShadeStats sa[2] = {a.clusteredStats, a.bruteStats};
    const DeferredShadeStats sb[2] = {b.clusteredStats, b.bruteStats};
    for (u32 i = 0; i < 2u; ++i) {
        const bool same = sa[i].shadedPixels == sb[i].shadedPixels && sa[i].skippedPixels == sb[i].skippedPixels &&
                          sa[i].lightEvaluations == sb[i].lightEvaluations;
        r.mismatches += same ? 0u : 1u;
    }
    r.ok = r.launches_ok && r.mismatches == 0u;
    const bool cullSame = a.cull.lightsCulled == b.cull.lightsCulled &&
                          a.cull.clustersAtCapacity == b.cull.clustersAtCapacity &&
                          a.cull.lightsDroppedOverflow == b.cull.lightsDroppedOverflow;
    r.ok = r.ok && cullSame;
    return r;
}

void testBackendParity() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (f32 lightScale : {1.f, 0.5f}) {
        kernel::LoadScale scale = kernel::load_scale();
        scale.lights = lightScale;
        const kernel::ScopedLoadScale scoped(scale);
        for (u32 variant = 0; variant < 2u; ++variant) {
            const ClusterDesc desc = makeDesc(variant == 0u ? 8u : 0u); // capped (overflow) / unlimited
            const ClusterCameraDesc camera = makeCamera(variant == 0u);
            const Scene scene = makeScene(camera);
            expectTrue(scene.points.size() == kernel::scaled_count(600u, lightScale), "LoadScale::lights sizes the scene");
            scheduler.shutdown();
            const Frame reference = render(kernel::Backend::CpuReference, desc, camera, scene);
            expectTrue(reference.clusteredStats.shadedPixels > kWidth * kHeight / 2u &&
                           reference.clusteredStats.skippedPixels > 0u,
                       "frame has shaded and skipped pixels");
            expectTrue(reference.cull.lightsCulled > 0u && (variant != 0u || reference.cull.clustersAtCapacity > 0u),
                       "cull assigns lights (and overflows the capped grid)");
            if (variant == 1u) {
                // Unlimited lists: clustered shading must be bit-identical to looping every light.
                expectTrue(kernel::compare_bitwise(std::span<const Vec3>(reference.clustered),
                                                   std::span<const Vec3>(reference.bruteForce))
                               .ok,
                           "clustered shading == all-lights shading (no light omitted)");
            }
            for (u32 workers : {0u, 2u, 4u}) {
                scheduler.shutdown();
                scheduler.initialize(workers);
                const Frame parallel = render(kernel::Backend::CpuParallel, desc, camera, scene);
                const kernel::ParityReport report = compareFrames(reference, parallel);
                char label[160];
                std::snprintf(label, sizeof(label),
                              "clustered CpuReference == CpuParallel bit-exact (%u workers, variant %u, lights x%.1f, "
                              "%llu mismatches)",
                              workers, variant, static_cast<double>(lightScale),
                              static_cast<unsigned long long>(report.mismatches));
                expectTrue(report.ok && report.compared > 0u, label);
            }
        }
    }
    scheduler.shutdown();
}

void testListsMatchVectorApi() {
    const ClusterDesc desc = makeDesc(8u);
    const ClusterCameraDesc camera = makeCamera(true);
    const Scene scene = makeScene(camera);
    std::vector<ClusterAABB> aabbs;
    cluster_math::buildClusterAabbs(desc, camera, aabbs, kernel::Backend::CpuReference);
    ClusterCullLists lists;
    std::vector<std::vector<u32>> perCluster;
    const ClusterCullResult a = cluster_math::cullLightsToClusterLists(desc, camera, aabbs, scene.points, scene.spots,
                                                                       lists, kernel::Backend::CpuReference);
    const ClusterCullResult b =
        cluster_math::cullLightsToClusters(desc, camera, aabbs, scene.points, scene.spots, perCluster);
    bool same = a.lightsCulled == b.lightsCulled && a.lightsDroppedOverflow == b.lightsDroppedOverflow &&
                perCluster.size() == desc.clusterCount();
    for (u32 c = 0; same && c < desc.clusterCount(); ++c) {
        same = perCluster[c].size() == lists.counts[c];
        for (u32 i = 0; same && i < lists.counts[c]; ++i) {
            same = perCluster[c][i] == lists.slots[c * lists.capacity + i] &&
                   (i == 0u || perCluster[c][i - 1u] < perCluster[c][i]);
        }
    }
    expectTrue(same, "vector cull API == fixed-capacity lists (ascending light index)");

    // Compaction == the serial per-cluster packer.
    ClusterGridSoA kernelGrid;
    ClusterGridSoA serialGrid;
    cluster_math::compactClusterLists(lists, desc.clusterCount(), kernelGrid, kernel::Backend::CpuParallel);
    ClusterLightGridLayout::rebuildLightGrid(serialGrid, desc.clusterCount(), perCluster, desc.maxLightsPerCluster);
    expectTrue(kernel::compare_bitwise(std::span<const ClusterGridEntry>(kernelGrid.grid),
                                       std::span<const ClusterGridEntry>(serialGrid.grid))
                       .ok &&
                   kernelGrid.lightList == serialGrid.lightList &&
                   ClusterLightGridLayout::validateContiguousOffsets(kernelGrid, desc.clusterCount()),
               "count -> scan -> compact == serial light-grid rebuild");
}

void testWideCounter() {
    u32 words[2] = {0xFFFFFFF0u, 7u};
    clustered_kernel::add_u64_counter(words, 0x20u);
    clustered_kernel::add_u64_counter(words, 5u);
    const u64 value = (static_cast<u64>(words[1]) << 32u) | words[0];
    expectTrue(value == (7ull << 32u) + 0xFFFFFFF0ull + 0x25ull, "64-bit evaluation counter carries across u32 wrap");
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    const ClusterDesc desc = makeDesc(8u);
    const ClusterCameraDesc camera = makeCamera(true);
    const Scene scene = makeScene(camera);
    kernel::reset_kernel_stats();
    const Frame reference = render(kernel::Backend::CpuReference, desc, camera, scene);

    const u32 clusters = desc.clusterCount();
    const u32 lights = static_cast<u32>(scene.points.size() + scene.spots.size());
    const u64 clusterGroups = (clusters + 63u) / 64u;
    struct Expect {
        const char* name;
        u64 launches;
        u64 items;
        u64 workgroups;
    };
    const Expect expected[] = {
        {clustered_kernel::kBuildName, 1u, clusters, clusterGroups},
        {clustered_kernel::kBoundsName, 1u, lights, (lights + 63u) / 64u},
        {clustered_kernel::kBinName, 1u, desc.slicesZ, (desc.slicesZ + 63u) / 64u},
        {clustered_kernel::kCullName, 1u, clusters, clusterGroups},
        {clustered_kernel::kCompactName, 1u, clusters, clusterGroups},
        {clustered_kernel::kShadeName, 2u, 2u * kWidth * kHeight, 2u * ((kWidth + 7u) / 8u) * ((kHeight + 7u) / 8u)},
    };
    for (const Expect& e : expected) {
        kernel::KernelStats stats{};
        char label[128];
        std::snprintf(label, sizeof(label), "%s stats (launches, items, workgroups, backend)", e.name);
        expectTrue(kernel::find_kernel_stats(e.name, stats) && stats.launches == e.launches && stats.items == e.items &&
                       stats.workgroups == e.workgroups && stats.last_backend == kernel::Backend::CpuReference,
                   label);
    }

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const Frame fallback = render(gpu, desc, camera, scene);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel &&
                           last.name != nullptr && std::string_view(last.name) == clustered_kernel::kShadeName,
                       "GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(compareFrames(reference, fallback).ok, "fallback frame == CpuReference frame");
        }
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testListsMatchVectorApi();
    testWideCounter();
    testStatsAndFallback();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d clustered kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Clustered kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
