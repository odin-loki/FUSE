// Gate for the reference single-source kernel port (fuse/compute/ray_march_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical depth and normal images of a
//     scene with every primitive type, smooth + hard unions and misses.
//   - The per-pixel kernel output equals the public scalar API (ray_march_hit_distance /
//     ray_march_scene_normal) — one implementation, no divergent copies.
//   - Launches record "sdf_ray_march" stats; Cuda / Auto without a device fall back to CpuParallel
//     with the same image; invalid params are rejected on every backend.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
namespace compute = fuse::compute;
namespace kernel = fuse::kernel;
using fuse::math::Vec3;
using fuse::math::Vec4;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

compute::SdfObject makeObject(compute::SdfPrimitiveType type, Vec3 position, Vec3 params, f32 alpha,
                              f32 rounding = 0.f) {
    compute::SdfObject obj{};
    obj.type = static_cast<u32>(type);
    obj.position = position;
    obj.params = params;
    obj.alpha = alpha;
    obj.rounding = rounding;
    return obj;
}

const std::vector<compute::SdfObject>& sceneObjects() {
    static const std::vector<compute::SdfObject> objects{
        makeObject(compute::SdfPrimitiveType::Sphere, {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.f),
        makeObject(compute::SdfPrimitiveType::Box, {1.6f, -0.3f, 0.4f}, {0.6f, 0.5f, 0.7f}, 0.35f, 0.15f),
        makeObject(compute::SdfPrimitiveType::Capsule, {-1.8f, 0.2f, 0.5f}, {0.35f, 0.8f, 0.f}, 0.25f),
        makeObject(compute::SdfPrimitiveType::Torus, {0.2f, 1.3f, 1.0f}, {0.8f, 0.2f, 0.f}, 0.f),
        makeObject(compute::SdfPrimitiveType::Sphere, {0.4f, -1.1f, -0.6f}, {0.5f, 0.f, 0.f}, 0.5f),
    };
    return objects;
}

compute::RayMarchParams makeScene(u32 width, u32 height) {
    compute::RayMarchParams params{};
    params.width = width;
    params.height = height;
    params.cam_pos = {0.3f, 0.4f, -5.f};
    params.cam_forward = {0.f, -0.05f, 1.f};
    params.cam_right = {1.f, 0.f, 0.f};
    params.cam_up = {0.f, 1.f, 0.05f};
    params.fov_rad = 1.1f;
    params.max_steps = 160;
    params.min_dist = 0.0005f;
    params.max_dist = 50.f;
    params.objects = sceneObjects().data();
    params.object_count = static_cast<u32>(sceneObjects().size());
    return params;
}

struct Image {
    std::vector<f32> depth;
    std::vector<Vec4> normal;
};

Image render(kernel::Backend backend, u32 width, u32 height, bool viaOn = false) {
    Image image{std::vector<f32>(width * height, -7.f), std::vector<Vec4>(width * height, Vec4{9.f, 9.f, 9.f, 9.f})};
    compute::RayMarchParams params = makeScene(width, height);
    params.depth_surface = image.depth.data();
    params.output_surface = image.normal.data();
    const bool ok = viaOn ? compute::launch_ray_march_on(backend, params)
                          : compute::launch_ray_march_cpu_backend(backend, params);
    expectTrue(ok, "ray march launch succeeds");
    return image;
}

kernel::ParityReport compareImages(const Image& a, const Image& b) {
    kernel::ParityReport report =
        kernel::compare_bitwise(std::span<const f32>(a.depth), std::span<const f32>(b.depth));
    report.merge(kernel::compare_bitwise(std::span<const Vec4>(a.normal), std::span<const Vec4>(b.normal)));
    return report;
}

void testBackendParity() {
    constexpr u32 kW = 173; // not a multiple of the 8x8 workgroup: partial edge tiles
    constexpr u32 kH = 97;
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const Image reference = render(kernel::Backend::CpuReference, kW, kH);

    u32 hits = 0;
    u32 misses = 0;
    for (f32 d : reference.depth) {
        (d >= 0.f ? hits : misses) += 1u;
    }
    expectTrue(hits > kW * kH / 10u && misses > kW * kH / 10u, "scene has both hits and misses");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const Image parallel = render(kernel::Backend::CpuParallel, kW, kH);
        const kernel::ParityReport report = compareImages(reference, parallel);
        char label[128];
        std::snprintf(label, sizeof(label),
                      "CpuReference == CpuParallel bit-exact depth + normals (%u workers, %llu mismatches)", workers,
                      static_cast<unsigned long long>(report.mismatches));
        expectTrue(report.ok && report.compared == kW * kH * 2u, label);
    }

    // The kernel body and the public scalar API are the same code.
    const compute::RayMarchParams scene = makeScene(kW, kH);
    const compute::ray_march_kernel::Params kp = compute::ray_march_kernel::make_params(scene);
    bool same = true;
    for (u32 y = 0; y < kH; y += 7) {
        for (u32 x = 0; x < kW; x += 5) {
            const f32 ndcX = (2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kW) - 1.f);
            const f32 ndcY = (1.f - 2.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kH));
            const Vec3 dir = (kp.forward + kp.right * (ndcX * kp.tan_half * kp.aspect) + kp.up * (ndcY * kp.tan_half))
                                 .normalized();
            const f32 t = compute::ray_march_hit_distance(scene, scene.cam_pos, dir);
            const u32 i = y * kW + x;
            // Near rather than bitwise: this TU recomputes the pixel direction itself, and a compiler
            // may contract (FMA) it differently here than inside fuse_compute.
            same = same && (t >= 0.f) == (reference.depth[i] >= 0.f) &&
                   std::fabs(t - reference.depth[i]) <= 1e-4f * std::fmax(1.f, std::fabs(t));
            if (t >= 0.f && reference.depth[i] >= 0.f) {
                const Vec3 n = compute::ray_march_scene_normal(scene, scene.cam_pos + dir * t);
                same = same && std::fabs(n.x - reference.normal[i].x) < 1e-3f &&
                       std::fabs(n.y - reference.normal[i].y) < 1e-3f &&
                       std::fabs(n.z - reference.normal[i].z) < 1e-3f && reference.normal[i].w == 1.f;
            }
        }
    }
    expectTrue(same, "kernel pixels == public ray_march_hit_distance / ray_march_scene_normal");
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    constexpr u32 kW = 64;
    constexpr u32 kH = 40;
    const Image reference = render(kernel::Backend::CpuReference, kW, kH, true);

    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(compute::ray_march_kernel::kName, stats) && stats.launches == 1u &&
                   stats.items == kW * kH && stats.workgroups == 8u * 5u &&
                   stats.last_backend == kernel::Backend::CpuReference,
               "launch records sdf_ray_march stats (items, 8x8 workgroups, backend)");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const Image fallback = render(gpu, kW, kH, true);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(compareImages(reference, fallback).ok, "fallback image == CpuReference image");
        }
        const Image viaLaunch = [&] {
            Image image{std::vector<f32>(kW * kH), std::vector<Vec4>(kW * kH)};
            compute::RayMarchParams params = makeScene(kW, kH);
            params.depth_surface = image.depth.data();
            params.output_surface = image.normal.data();
            expectTrue(compute::launch_ray_march(params), "launch_ray_march succeeds");
            return image;
        }();
        expectTrue(compareImages(reference, viaLaunch).ok, "launch_ray_march (Auto) image == CpuReference image");
        expectTrue(!compute::ray_marcher_info().device_available, "ray_marcher_info: no device");
    } else {
        // Device present: CUDA transcendental functions differ by ulps — tolerance parity.
        const Image gpu = render(kernel::Backend::Cuda, kW, kH, true);
        kernel::ParityReport report = kernel::compare_floats(std::span<const f32>(reference.depth),
                                                             std::span<const f32>(gpu.depth), {1e-3, 1e-4});
        expectTrue(report.mismatches <= kW * kH / 200u, "CUDA depth within tolerance of CpuReference");
    }

    compute::RayMarchParams invalid = makeScene(kW, kH);
    invalid.fov_rad = 0.f;
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda,
                              kernel::Backend::Auto}) {
        expectTrue(!compute::launch_ray_march_on(b, invalid), "invalid params rejected on every backend");
    }
    scheduler.shutdown();
}

void timeCuda1080() {
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        std::printf("sdf_ray_march 1080p: CUDA device not available\n");
        return;
    }
    const std::vector<compute::SdfObject> base = sceneObjects();
    std::vector<compute::SdfObject> objects = base;
    objects.insert(objects.end(), base.begin(), base.end());
    constexpr u32 kW = 1920;
    constexpr u32 kH = 1080;
    std::vector<f32> depth(static_cast<size_t>(kW) * kH);
    std::vector<Vec4> normal(static_cast<size_t>(kW) * kH);
    compute::RayMarchParams params = makeScene(kW, kH);
    params.objects = objects.data();
    params.object_count = static_cast<u32>(objects.size());
    params.depth_surface = depth.data();
    params.output_surface = normal.data();
    expectTrue(compute::launch_ray_march_on(kernel::Backend::Cuda, params), "1080p cuda warmup");
    kernel::reset_kernel_stats();
    const auto t0 = std::chrono::steady_clock::now();
    expectTrue(compute::launch_ray_march_on(kernel::Backend::Cuda, params), "1080p cuda timed");
    const auto t1 = std::chrono::steady_clock::now();
    kernel::KernelStats ks{};
    const bool have = kernel::find_kernel_stats("sdf_ray_march", ks);
    const double wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("sdf_ray_march 1920x1080 %u objects CUDA: wall %.2f ms (upload+launch+download), kernel last %.2f ms\n",
                params.object_count, wallMs, have ? static_cast<double>(ks.last_ns) / 1e6 : -1.0);
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testStatsAndFallback();
    timeCuda1080();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d ray march kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Ray march kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
