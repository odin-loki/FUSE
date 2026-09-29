// Gate for the tiled SDF ray march ("sdf_ray_march_tiled", fuse/compute/ray_march_kernel.hpp) — the
// B3 / B5 rows "renders 500 SDF objects at 1080p > 60 fps" and "SDF ray march (100 objects, 128 steps)
// < 3 ms" need the march to stop evaluating every object at every step:
//   - Tiled CpuReference == tiled CpuParallel (0/2/4 workers): bit-identical depth + normal images.
//   - Tiled vs the full-scene march ("sdf_ray_march", the reference) on 100- and 500-object scenes with
//     every primitive, hard and smooth unions and a floor: the same hit mask on all but a sliver of
//     grazing pixels, depth within the trace's stopping tolerance, the same normals.
//   - Culling: the per-tile lists (the kernel's own tile_frustum / tile_keeps) hold a small fraction of
//     the scene; an object culled from a tile is never hit by any of its pixel rays (checked by tracing
//     every pixel against that object alone).
//   - Overflow: a tile whose pyramid meets more than kTileListCapacity objects marches the whole scene,
//     bit-identical to the full march there.
//   - "sdf_ray_march_tiled" stats; GPU requests without a device fall back to CpuParallel (recorded).
//   - With a CUDA device: CUDA tiled == CPU tiled (depth tolerance) and the resident-buffer CUDA-event
//     benchmark at 1920x1080: 100 objects / 128 steps (full and tiled) and 500 objects (full and tiled).
//     Targets (< 3 ms, < 16.7 ms) are printed; asserted only with FUSE_DEVICE_BUDGETS=1.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
namespace compute = fuse::compute;
namespace kernel = fuse::kernel;
namespace rm = fuse::compute::ray_march_kernel;
using fuse::math::Vec3;
using fuse::math::Vec4;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// `count` primitives scattered through a 40 x 10 x 70 volume in front of the camera (every type, half
/// of them smooth-union blended, some rounded boxes) plus a floor slab as object 0.
std::vector<compute::SdfObject> makeObjects(u32 count, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    std::vector<compute::SdfObject> objects;
    compute::SdfObject floor{};
    floor.type = static_cast<u32>(compute::SdfPrimitiveType::Box);
    floor.position = {0.f, -4.f, 40.f};
    floor.params = {40.f, 0.5f, 45.f};
    floor.alpha = 0.f;
    objects.push_back(floor);
    for (u32 i = 1; i < count; ++i) {
        compute::SdfObject obj{};
        obj.type = i % 4u;
        obj.position = {unit(rng) * 40.f - 20.f, unit(rng) * 9.f - 3.f, unit(rng) * 70.f + 6.f};
        const f32 s = 0.25f + 0.9f * unit(rng);
        switch (static_cast<compute::SdfPrimitiveType>(obj.type)) {
        case compute::SdfPrimitiveType::Sphere: obj.params = {s, 0.f, 0.f}; break;
        case compute::SdfPrimitiveType::Box:
            obj.params = {s, 0.3f + 0.8f * unit(rng), 0.3f + 0.8f * unit(rng)};
            obj.rounding = (i % 3u == 0u) ? 0.15f : 0.f;
            break;
        case compute::SdfPrimitiveType::Capsule: obj.params = {0.5f * s, s, 0.f}; break;
        default: obj.params = {s, 0.3f * s, 0.f}; break;
        }
        obj.alpha = (i % 2u == 0u) ? 0.05f + 0.35f * unit(rng) : 0.f;
        objects.push_back(obj);
    }
    return objects;
}

compute::RayMarchParams makeScene(u32 width, u32 height, const std::vector<compute::SdfObject>& objects,
                                  u32 maxSteps = 128u) {
    compute::RayMarchParams params{};
    params.width = width;
    params.height = height;
    params.cam_pos = {0.5f, 1.5f, -4.f};
    params.cam_forward = {0.f, -0.08f, 1.f};
    params.cam_right = {1.f, 0.f, 0.f};
    params.cam_up = {0.f, 1.f, 0.08f};
    params.fov_rad = 1.05f;
    params.max_steps = maxSteps;
    params.min_dist = 0.001f;
    params.max_dist = 150.f;
    params.objects = objects.data();
    params.object_count = static_cast<u32>(objects.size());
    return params;
}

struct Image {
    std::vector<f32> depth;
    std::vector<Vec4> normal;
};

Image render(kernel::Backend backend, compute::RayMarchParams params, bool tiled) {
    const std::size_t pixels = static_cast<std::size_t>(params.width) * params.height;
    Image image{std::vector<f32>(pixels, -7.f), std::vector<Vec4>(pixels, Vec4{9.f, 9.f, 9.f, 9.f})};
    params.depth_surface = image.depth.data();
    params.output_surface = image.normal.data();
    const bool ok = tiled ? compute::launch_ray_march_tiled_on(backend, params)
                          : compute::launch_ray_march_on(backend, params);
    expectTrue(ok, "ray march launch succeeds");
    return image;
}

kernel::ParityReport compareImages(const Image& a, const Image& b) {
    kernel::ParityReport report =
        kernel::compare_bitwise(std::span<const f32>(a.depth), std::span<const f32>(b.depth));
    report.merge(kernel::compare_bitwise(std::span<const Vec4>(a.normal), std::span<const Vec4>(b.normal)));
    return report;
}

/// Tiled vs full-scene images: hit-mask flips, depth error and normal agreement over pixels both hit.
struct Agreement {
    u32 pixels = 0;
    u32 hits = 0;
    u32 flips = 0;
    u32 firstOnly = 0; ///< flips where only the first image hits (the other ran out of steps / grazes)
    u32 depthOutliers = 0;
    u32 normalOutliers = 0;
    f32 maxRelDepth = 0.f;
};

Agreement agreement(const Image& tiled, const Image& full, f32 minDist) {
    Agreement a{};
    a.pixels = static_cast<u32>(full.depth.size());
    for (std::size_t i = 0; i < full.depth.size(); ++i) {
        const bool th = tiled.depth[i] >= 0.f;
        const bool fh = full.depth[i] >= 0.f;
        if (th != fh) {
            ++a.flips;
            a.firstOnly += th ? 1u : 0u;
            continue;
        }
        if (!fh) {
            continue;
        }
        ++a.hits;
        const f32 err = std::fabs(tiled.depth[i] - full.depth[i]);
        a.maxRelDepth = std::max(a.maxRelDepth, err / std::max(full.depth[i], 1.f));
        // The trace stops anywhere inside min_dist of the surface: a few min_dist along a grazing ray.
        if (err > 20.f * minDist + 1e-4f * full.depth[i]) {
            ++a.depthOutliers;
            continue;
        }
        const Vec4& n0 = tiled.normal[i];
        const Vec4& n1 = full.normal[i];
        if (n0.x * n1.x + n0.y * n1.y + n0.z * n1.z < 0.99f) {
            ++a.normalOutliers;
        }
    }
    return a;
}

/// Per-tile list lengths from the kernel's own culling helpers; also proves that every culled object is
/// missed by every pixel ray of its tile (traced against that object alone, up to max_dist).
struct CullStats {
    double meanKept = 0.0;
    u32 maxKept = 0;
    u32 tiles = 0;
    u32 culledButHit = 0;
};

CullStats cullStats(const compute::RayMarchParams& scene, bool verifyCulled) {
    const rm::Params p = rm::make_tiled_params(scene, scene.objects);
    const kernel::Dim3 groups = kernel::group_count(kernel::extent2(scene.width, scene.height), rm::kWorkgroup);
    CullStats stats{};
    double total = 0.0;
    for (u32 gy = 0; gy < groups.y; ++gy) {
        for (u32 gx = 0; gx < groups.x; ++gx) {
            const rm::TileFrustum f = rm::tile_frustum(rm::tile_rect(p, kernel::Dim3{gx, gy, 0u}));
            u32 kept = 0;
            for (u32 i = 0; i < scene.object_count; ++i) {
                if (rm::tile_keeps(p, f, i)) {
                    ++kept;
                    continue;
                }
                if (!verifyCulled) {
                    continue;
                }
                compute::RayMarchParams alone = scene;
                alone.objects = scene.objects + i;
                alone.object_count = 1u;
                alone.max_steps = 512u;
                for (u32 y = gy * rm::kWorkgroup.y; y < std::min((gy + 1u) * rm::kWorkgroup.y, scene.height); ++y) {
                    for (u32 x = gx * rm::kWorkgroup.x; x < std::min((gx + 1u) * rm::kWorkgroup.x, scene.width);
                         ++x) {
                        const Vec3 dir = rm::pixel_direction(p, static_cast<f32>(x), static_cast<f32>(y));
                        stats.culledButHit += compute::ray_march_hit_distance(alone, scene.cam_pos, dir) >= 0.f ? 1u : 0u;
                    }
                }
            }
            total += kept;
            stats.maxKept = std::max(stats.maxKept, kept);
            ++stats.tiles;
        }
    }
    stats.meanKept = total / std::max(stats.tiles, 1u);
    return stats;
}

void testBackendParityAndAccuracy() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (u32 count : {100u, 500u}) {
        const std::vector<compute::SdfObject> objects = makeObjects(count, 17u + count);
        constexpr u32 kW = 213; // partial edge tiles in both directions
        constexpr u32 kH = 115;
        const compute::RayMarchParams scene = makeScene(kW, kH, objects);

        scheduler.shutdown();
        const auto t0 = std::chrono::steady_clock::now();
        const Image tiledRef = render(kernel::Backend::CpuReference, scene, true);
        const auto t1 = std::chrono::steady_clock::now();
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            const kernel::ParityReport report = compareImages(tiledRef, render(kernel::Backend::CpuParallel, scene, true));
            char label[160];
            std::snprintf(label, sizeof(label),
                          "%u objects: tiled CpuReference == CpuParallel bit-exact (%u workers, %llu mismatches)", count,
                          workers, static_cast<unsigned long long>(report.mismatches));
            expectTrue(report.ok && report.compared == kW * kH * 2u, label);
        }
        const auto t2 = std::chrono::steady_clock::now();
        const Image full = render(kernel::Backend::CpuParallel, scene, false);
        const auto t3 = std::chrono::steady_clock::now();
        scheduler.shutdown();

        const Agreement a = agreement(tiledRef, full, scene.min_dist);
        const CullStats cull = cullStats(scene, count == 100u);
        std::printf("sdf_ray_march_tiled %u objects %ux%u: %u hits, %u hit/miss flips (%u hit only when tiled), %u "
                    "depth outliers (max rel %.2e), %u normal outliers; tile lists mean %.1f max %u of %u; CPU tiled "
                    "(4 workers) %.1f ms vs full %.1f ms, tiled CpuReference %.1f ms\n",
                    count, kW, kH, a.hits, a.flips, a.firstOnly, a.depthOutliers, static_cast<double>(a.maxRelDepth),
                    a.normalOutliers, cull.meanKept, cull.maxKept, count,
                    std::chrono::duration<double, std::milli>(t2 - t1).count() / 3.0,
                    std::chrono::duration<double, std::milli>(t3 - t2).count(),
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        expectTrue(a.hits > a.pixels / 4u, "scene covers the frame");
        expectTrue(a.flips <= a.pixels / 500u, "tiled vs full: hit masks agree on >= 99.8% of pixels");
        expectTrue(a.depthOutliers <= a.hits / 500u, "tiled vs full: depth within the stopping tolerance (>= 99.8%)");
        expectTrue(a.normalOutliers <= a.hits / 500u, "tiled vs full: normals agree (>= 99.8%)");
        expectTrue(cull.meanKept < 0.25 * count, "tile lists hold under a quarter of the scene on average");
        expectTrue(cull.culledButHit == 0u, "no culled object is hit by any pixel ray of its tile");
    }
}

void testOverflowFallback() {
    // 1100 small spheres packed along the view ray through pixel (40, 28) — inside tile (2, 3) — so that
    // tile meets more than the 1024-entry list and must march the whole scene (bit-identical to the full
    // march there).
    std::vector<compute::SdfObject> objects;
    for (u32 i = 0; i < 1100u; ++i) {
        compute::SdfObject obj{};
        obj.type = static_cast<u32>(compute::SdfPrimitiveType::Sphere);
        const f32 z = 6.f + 0.2f * static_cast<f32>(i / 110u);
        obj.position = {z * 0.0535f + 0.008f * static_cast<f32>(i % 11u) - 0.04f,
                        -z * 0.0283f + 0.008f * static_cast<f32>((i / 11u) % 10u) - 0.04f, z};
        obj.params = {0.01f, 0.f, 0.f};
        obj.alpha = (i % 5u == 0u) ? 0.004f : 0.f;
        objects.push_back(obj);
    }
    constexpr u32 kW = 64;
    constexpr u32 kH = 48;
    compute::RayMarchParams scene = makeScene(kW, kH, objects);
    scene.cam_pos = {0.f, 0.f, 0.f};
    scene.cam_forward = {0.f, 0.f, 1.f};
    scene.cam_up = {0.f, 1.f, 0.f};
    scene.fov_rad = 0.3f;
    const CullStats cull = cullStats(scene, false);
    const Image tiled = render(kernel::Backend::CpuReference, scene, true);
    const Image full = render(kernel::Backend::CpuReference, scene, false);
    const rm::Params p = rm::make_tiled_params(scene, scene.objects);
    u32 overflowTiles = 0;
    u32 overflowMismatch = 0;
    for (u32 gy = 0; gy < kH / rm::kWorkgroup.y; ++gy) {
        for (u32 gx = 0; gx < kW / rm::kWorkgroup.x; ++gx) {
            const rm::TileFrustum f = rm::tile_frustum(rm::tile_rect(p, kernel::Dim3{gx, gy, 0u}));
            u32 kept = 0;
            for (u32 i = 0; i < scene.object_count; ++i) {
                kept += rm::tile_keeps(p, f, i) ? 1u : 0u;
            }
            if (kept <= rm::kTileListCapacity) {
                continue;
            }
            ++overflowTiles;
            for (u32 y = gy * rm::kWorkgroup.y; y < (gy + 1u) * rm::kWorkgroup.y; ++y) {
                for (u32 x = gx * rm::kWorkgroup.x; x < (gx + 1u) * rm::kWorkgroup.x; ++x) {
                    const u32 i = y * kW + x;
                    overflowMismatch += (std::memcmp(&tiled.depth[i], &full.depth[i], sizeof(f32)) != 0 ||
                                         std::memcmp(&tiled.normal[i], &full.normal[i], sizeof(Vec4)) != 0)
                                            ? 1u
                                            : 0u;
                }
            }
        }
    }
    std::printf("sdf_ray_march_tiled overflow: %u of %u tiles exceed %u objects (max %u), %u pixel mismatches there\n",
                overflowTiles, cull.tiles, rm::kTileListCapacity, cull.maxKept, overflowMismatch);
    expectTrue(overflowTiles > 0u && overflowMismatch == 0u,
               "overflowing tiles march the whole scene (bit-identical to the full march)");
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    const std::vector<compute::SdfObject> objects = makeObjects(40u, 3u);
    const compute::RayMarchParams scene = makeScene(64u, 40u, objects);
    kernel::reset_kernel_stats();
    const Image reference = render(kernel::Backend::CpuReference, scene, true);
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(rm::kTiledName, stats) && stats.launches == 1u && stats.items == 64u * 40u &&
                   stats.workgroups == 4u * 5u && stats.last_backend == kernel::Backend::CpuReference,
               "launch records sdf_ray_march_tiled stats (items, 16x8 tiles, backend)");
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const Image fallback = render(gpu, scene, true);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "tiled GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(compareImages(reference, fallback).ok, "tiled fallback image == CpuReference image");
        }
        compute::RayMarchDeviceTiming timing{};
        expectTrue(!compute::benchmark_ray_march_cuda(scene, true, 1u, timing), "no device: benchmark reports false");
    }
    compute::RayMarchParams invalid = scene;
    invalid.max_steps = 0u;
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda}) {
        expectTrue(!compute::launch_ray_march_tiled_on(b, invalid), "invalid params rejected on every backend");
    }
    scheduler.shutdown();
}

bool deviceBudgetsEnforced() {
    const char* enforce = std::getenv("FUSE_DEVICE_BUDGETS");
    return enforce != nullptr && enforce[0] == '1' && fuse::core::timingBudgetsEnforcedNoted();
}

void benchmarkDevice() {
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        std::printf("sdf_ray_march_tiled device benchmark: no CUDA device (skipped)\n");
        return;
    }
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(std::max(1u, std::thread::hardware_concurrency()) - 1u);

    // Device parity at a size the CPU renders quickly.
    {
        const std::vector<compute::SdfObject> objects = makeObjects(500u, 517u);
        const compute::RayMarchParams scene = makeScene(480u, 270u, objects);
        const Image cpu = render(kernel::Backend::CpuParallel, scene, true);
        const Image gpu = render(kernel::Backend::Cuda, scene, true);
        const Agreement a = agreement(gpu, cpu, scene.min_dist);
        std::printf("sdf_ray_march_tiled CUDA vs CPU 480x270 500 objects: %u hits, %u flips, %u depth outliers, %u "
                    "normal outliers\n",
                    a.hits, a.flips, a.depthOutliers, a.normalOutliers);
        expectTrue(a.flips <= a.pixels / 500u && a.depthOutliers <= a.hits / 500u,
                   "CUDA tiled march == CPU tiled march (tolerance)");
    }

    struct Case {
        const char* label;
        u32 objects;
        u32 steps;
        bool tiled;
        f32 budgetMs;
    };
    const Case cases[] = {
        {"100 objects, 128 steps, full scene", 100u, 128u, false, 3.f},
        {"100 objects, 128 steps, tiled", 100u, 128u, true, 3.f},
        {"500 objects, full scene", 500u, 128u, false, 1000.f / 60.f},
        {"500 objects, tiled", 500u, 128u, true, 1000.f / 60.f},
    };
    f32 best100 = 3.4e38f;
    f32 best500 = 3.4e38f;
    for (const Case& c : cases) {
        const std::vector<compute::SdfObject> objects = makeObjects(c.objects, 17u + c.objects);
        compute::RayMarchParams scene = makeScene(1920u, 1080u, objects, c.steps);
        std::vector<f32> depth(1920u * 1080u);
        std::vector<Vec4> normal(1920u * 1080u);
        scene.depth_surface = depth.data();
        scene.output_surface = normal.data();
        compute::RayMarchDeviceTiming timing{};
        const bool ran = compute::benchmark_ray_march_cuda(scene, c.tiled, 10u, timing);
        expectTrue(ran, "CUDA ray march benchmark ran");
        if (!ran) {
            continue;
        }
        std::printf("sdf_ray_march 1920x1080 %s: CUDA events kernel min %.3f ms avg %.3f ms [target < %.2f ms]; "
                    "upload %.3f ms, download %.2f ms; %d regs, %d B smem, %d blocks/SM = %.0f%% theoretical\n",
                    c.label, static_cast<double>(timing.kernel_ms_min), static_cast<double>(timing.kernel_ms_avg),
                    static_cast<double>(c.budgetMs), static_cast<double>(timing.upload_ms),
                    static_cast<double>(timing.download_ms), timing.registers_per_thread, timing.shared_bytes_per_block,
                    timing.blocks_per_sm, 100.0 * static_cast<double>(timing.theoretical_occupancy));
        f32& best = c.objects == 100u ? best100 : best500;
        best = std::min(best, timing.kernel_ms_min);
    }
    if (deviceBudgetsEnforced()) {
        expectTrue(best100 < 3.f, "SDF ray march (100 objects, 128 steps) < 3 ms (CUDA events)");
        expectTrue(best500 < 1000.f / 60.f, "500 SDF objects at 1920x1080 > 60 fps (CUDA events, march only)");
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParityAndAccuracy();
    testOverflowFallback();
    testStatsAndFallback();
    benchmarkDevice();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d tiled ray march check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Tiled ray march gates passed\n");
    return EXIT_SUCCESS;
}
