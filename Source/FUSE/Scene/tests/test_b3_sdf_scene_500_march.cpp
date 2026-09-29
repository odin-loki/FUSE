// B3 row "Renderer receives SceneData from scene and renders 500 SDF objects at 1080p > 60fps" — the
// scene -> SceneData -> SDF object buffer -> ray march path at 500 objects:
//   1. A SceneManager holds 500 visible SDF entities (spheres, boxes, capsules, tori) in front of the
//      camera, 40 outside its view and 10 hidden ones; buildFrame culls them into SceneData.sdf_objects.
//   2. SceneData packs into the ray marcher's object buffer (compute::SdfObject, hard union) — the same
//      layout fuse_b3_sdf_buffer_ray_march uploads through the renderer.
//   3. CPU: the tiled march ("sdf_ray_march_tiled": per-16x8-tile object culling) of that buffer matches
//      the full-scene march within the trace tolerance and is bit-identical on CpuReference and
//      CpuParallel; the per-tile lists stay a small fraction of the 500 objects.
//   4. With a CUDA device: CUDA == CPU for the tiled march, then the resident-buffer CUDA-event
//      benchmark at 1920x1080 (march only — no present; 60 fps = 16.7 ms, asserted only with
//      FUSE_DEVICE_BUDGETS=1). Without a device the device part prints a skip line.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/scene/scene_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
using fuse::ecs::EntityID;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace compute = fuse::compute;
namespace kernel = fuse::kernel;
namespace rm = fuse::compute::ray_march_kernel;

constexpr u32 kVisible = 500;
constexpr u32 kOutside = 40;
constexpr u32 kHidden = 10;
constexpr f32 kFovDeg = 70.f;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

u32 primitiveType(fuse::ecs::SDFPrimitive type) {
    switch (type) {
    case fuse::ecs::SDFPrimitive::Sphere: return static_cast<u32>(compute::SdfPrimitiveType::Sphere);
    case fuse::ecs::SDFPrimitive::Box: return static_cast<u32>(compute::SdfPrimitiveType::Box);
    case fuse::ecs::SDFPrimitive::Capsule: return static_cast<u32>(compute::SdfPrimitiveType::Capsule);
    case fuse::ecs::SDFPrimitive::Torus: return static_cast<u32>(compute::SdfPrimitiveType::Torus);
    default: return UINT32_MAX;
    }
}

/// SceneData -> ray-march object buffer (axis-aligned, unscaled, hard union; fails on anything else).
bool packSdfObjects(const fuse::ecs::SceneData& frame, std::vector<compute::SdfObject>& out) {
    out.assign(frame.sdf_objects.size(), compute::SdfObject{});
    for (std::size_t i = 0; i < frame.sdf_objects.size(); ++i) {
        const fuse::ecs::SceneSdfObject& item = frame.sdf_objects[i];
        const f32* m = item.transform.data.data();
        const bool axisAligned = std::abs(m[0] - 1.f) < 1e-6f && std::abs(m[5] - 1.f) < 1e-6f &&
                                 std::abs(m[10] - 1.f) < 1e-6f && std::abs(m[1]) < 1e-6f && std::abs(m[2]) < 1e-6f &&
                                 std::abs(m[4]) < 1e-6f && std::abs(m[6]) < 1e-6f && std::abs(m[8]) < 1e-6f &&
                                 std::abs(m[9]) < 1e-6f;
        const u32 type = primitiveType(item.type);
        if (!axisAligned || type == UINT32_MAX || item.op != fuse::ecs::SDFCsgOp::Union || item.roughness != 0.f) {
            return false;
        }
        compute::SdfObject& obj = out[i];
        obj.position = Vec3{m[12], m[13], m[14]};
        obj.params = Vec3{item.params.x, item.params.y, item.params.z};
        obj.type = type;
        obj.alpha = 0.f;
        obj.material_id = item.material_id;
    }
    return true;
}

EntityID addSdf(fuse::ecs::Registry& reg, fuse::ecs::SDFPrimitive type, const Vec3& position, const Vec3& params,
                u32 material, bool visible) {
    const EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    t.dirty = true;
    reg.add(id, t);
    fuse::ecs::SDFObject sdf{};
    sdf.type = type;
    sdf.op = fuse::ecs::SDFCsgOp::Union;
    sdf.params = {params.x, params.y, params.z, 0.f};
    sdf.material_id = material;
    sdf.visible = visible;
    reg.add(id, sdf);
    return id;
}

struct Image {
    std::vector<f32> depth;
    std::vector<Vec4> normal;
};

Image render(kernel::Backend backend, compute::RayMarchParams params, bool tiled) {
    const std::size_t pixels = static_cast<std::size_t>(params.width) * params.height;
    Image image{std::vector<f32>(pixels, -7.f), std::vector<Vec4>(pixels)};
    params.depth_surface = image.depth.data();
    params.output_surface = image.normal.data();
    expectTrue(tiled ? compute::launch_ray_march_tiled_on(backend, params) : compute::launch_ray_march_on(backend, params),
               "ray march launch succeeds");
    return image;
}

struct Agreement {
    u32 pixels = 0;
    u32 hits = 0;
    u32 flips = 0;
    u32 firstOnly = 0; ///< flips where only the first image hits (the other ran out of steps / grazes)
    u32 depthOutliers = 0;
    u32 normalOutliers = 0;
};

Agreement agreement(const Image& a, const Image& b, f32 minDist) {
    Agreement r{};
    r.pixels = static_cast<u32>(b.depth.size());
    for (std::size_t i = 0; i < b.depth.size(); ++i) {
        const bool ah = a.depth[i] >= 0.f;
        const bool bh = b.depth[i] >= 0.f;
        if (ah != bh) {
            ++r.flips;
            r.firstOnly += ah ? 1u : 0u;
            continue;
        }
        if (!bh) {
            continue;
        }
        ++r.hits;
        if (std::fabs(a.depth[i] - b.depth[i]) > 20.f * minDist + 1e-4f * b.depth[i]) {
            ++r.depthOutliers;
            continue;
        }
        const Vec4& n0 = a.normal[i];
        const Vec4& n1 = b.normal[i];
        r.normalOutliers += n0.x * n1.x + n0.y * n1.y + n0.z * n1.z < 0.99f ? 1u : 0u;
    }
    return r;
}

} // namespace

int main() {
    fuse::core::initialize();

    // --- ECS scene: 500 visible SDF entities in view, 40 outside it, 10 hidden ---
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc sceneDesc{};
    sceneDesc.hasVoxels = false;
    scene.init(sceneDesc);
    auto& reg = scene.registry();
    using P = fuse::ecs::SDFPrimitive;
    std::mt19937 rng(500u);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    const P types[] = {P::Sphere, P::Box, P::Capsule, P::Torus};
    (void)addSdf(reg, P::Box, {0.f, -3.f, 45.f}, {40.f, 0.5f, 45.f}, 1u, true); // floor
    for (u32 i = 1; i < kVisible; ++i) {
        // Inside the camera's view cone (camera at z = -10 looking down +z, 70 deg vertical fov).
        const f32 z = 8.f + 70.f * unit(rng);
        const f32 halfW = 0.55f * (z + 10.f);
        const f32 halfH = 0.4f * (z + 10.f);
        const Vec3 position{(2.f * unit(rng) - 1.f) * halfW * 0.8f, (2.f * unit(rng) - 1.f) * halfH * 0.5f + 1.f, z};
        const f32 s = 0.3f + 0.9f * unit(rng);
        const P type = types[i % 4u];
        const Vec3 params = type == P::Sphere    ? Vec3{s, 0.f, 0.f}
                            : type == P::Box     ? Vec3{s, 0.4f + 0.6f * unit(rng), 0.4f + 0.6f * unit(rng)}
                            : type == P::Capsule ? Vec3{0.5f * s, s, 0.f}
                                                 : Vec3{s, 0.3f * s, 0.f};
        (void)addSdf(reg, type, position, params, 2u + i, true);
    }
    for (u32 i = 0; i < kOutside; ++i) {
        const f32 side = i % 2u == 0u ? 1.f : -1.f;
        (void)addSdf(reg, P::Sphere, {side * (300.f + 10.f * static_cast<f32>(i)), 0.f, 20.f}, {1.f, 0.f, 0.f},
                     1000u + i, true);
    }
    for (u32 i = 0; i < kHidden; ++i) {
        (void)addSdf(reg, P::Sphere, {static_cast<f32>(i) - 5.f, 1.f, 12.f}, {0.8f, 0.f, 0.f}, 2000u + i, false);
    }
    const EntityID cameraId = scene.createCamera(kFovDeg, true);
    fuse::ecs::Camera* ecsCamera = reg.get<fuse::ecs::Camera>(cameraId);
    ecsCamera->aspect_ratio = 16.f / 9.f;
    ecsCamera->near_plane = 0.1f;
    ecsCamera->far_plane = 200.f;
    fuse::ecs::Transform* camT = reg.get<fuse::ecs::Transform>(cameraId);
    camT->position = {0.f, 1.f, -10.f, 1.f};
    camT->dirty = true;
    scene.update(0.f);

    const auto b0 = std::chrono::steady_clock::now();
    fuse::ecs::CullResult cull{};
    const fuse::ecs::SceneData frame = scene.buildFrame(&cull);
    const auto b1 = std::chrono::steady_clock::now();
    std::vector<compute::SdfObject> objects;
    const bool packed = packSdfObjects(frame, objects);
    expectTrue(packed, "SceneData packs into the ray-march object buffer");
    std::printf("SceneData: %zu SDF objects of %u entities (culled %u), buildFrame %.3f ms\n", frame.sdf_objects.size(),
                kVisible + kOutside + kHidden, cull.culled_count,
                std::chrono::duration<double, std::milli>(b1 - b0).count());
    expectTrue(frame.sdf_objects.size() == kVisible, "SceneData holds exactly the 500 visible in-view SDF objects");

    // Ray-march camera = the ECS camera's world frame (columns of local_to_world; forward = +Z).
    const f32* m = reg.get<fuse::ecs::Transform>(cameraId)->local_to_world.data.data();
    compute::RayMarchParams params{};
    params.cam_pos = Vec3{m[12], m[13], m[14]};
    params.cam_forward = Vec3{m[8], m[9], m[10]}.normalized();
    params.cam_right = Vec3{m[0], m[1], m[2]}.normalized();
    params.cam_up = Vec3{m[4], m[5], m[6]}.normalized();
    params.fov_rad = kFovDeg * 3.14159265358979f / 180.f;
    params.max_steps = 128;
    params.min_dist = 1e-3f;
    params.max_dist = 150.f;
    params.objects = objects.data();
    params.object_count = static_cast<u32>(objects.size());

    // --- CPU: tiled vs full-scene march, backend parity, list sizes ---
    params.width = 384;
    params.height = 216;
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(std::max(1u, std::thread::hardware_concurrency()) - 1u);
    const auto t0 = std::chrono::steady_clock::now();
    const Image tiled = render(kernel::Backend::CpuParallel, params, true);
    const auto t1 = std::chrono::steady_clock::now();
    const Image full = render(kernel::Backend::CpuParallel, params, false);
    const auto t2 = std::chrono::steady_clock::now();
    scheduler.shutdown();
    const Image tiledRef = render(kernel::Backend::CpuReference, params, true);
    kernel::ParityReport report =
        kernel::compare_bitwise(std::span<const f32>(tiledRef.depth), std::span<const f32>(tiled.depth));
    report.merge(kernel::compare_bitwise(std::span<const Vec4>(tiledRef.normal), std::span<const Vec4>(tiled.normal)));
    expectTrue(report.ok, "tiled march: CpuReference == CpuParallel bit-exact");

    const rm::Params tp = rm::make_tiled_params(params, params.objects);
    const kernel::Dim3 groups = kernel::group_count(kernel::extent2(params.width, params.height), rm::kWorkgroup);
    double keptSum = 0.0;
    u32 keptMax = 0;
    for (u32 gy = 0; gy < groups.y; ++gy) {
        for (u32 gx = 0; gx < groups.x; ++gx) {
            const rm::TileFrustum f = rm::tile_frustum(rm::tile_rect(tp, kernel::Dim3{gx, gy, 0u}));
            u32 kept = 0;
            for (u32 i = 0; i < params.object_count; ++i) {
                kept += rm::tile_keeps(tp, f, i) ? 1u : 0u;
            }
            keptSum += kept;
            keptMax = std::max(keptMax, kept);
        }
    }
    const double keptMean = keptSum / static_cast<double>(groups.x * groups.y);
    const Agreement a = agreement(tiled, full, params.min_dist);
    std::printf("SceneData 500-object march %ux%u: %u hit px, %u hit/miss flips (%u hit only when tiled), %u depth "
                "outliers, %u normal outliers; tile lists mean %.1f max %u of %u; CPU tiled %.1f ms vs full %.1f ms\n",
                params.width, params.height, a.hits, a.flips, a.firstOnly, a.depthOutliers, a.normalOutliers, keptMean,
                keptMax,
                params.object_count, std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count());
    expectTrue(a.hits > a.pixels / 4u, "the 500 objects cover the frame");
    expectTrue(a.flips <= a.pixels / 500u && a.depthOutliers <= a.hits / 500u && a.normalOutliers <= a.hits / 500u,
               "tiled march == full-scene march (hit mask, depth, normals on >= 99.8% of pixels)");
    expectTrue(keptMean < 0.1 * params.object_count, "tile lists hold under a tenth of the 500 objects on average");

    // --- CUDA device ---
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        std::printf("SceneData 500-object march 1920x1080: no CUDA device (device benchmark skipped)\n");
    } else {
        const Image gpu = render(kernel::Backend::Cuda, params, true);
        const Agreement g = agreement(gpu, tiled, params.min_dist);
        std::printf("SceneData 500-object march CUDA vs CPU %ux%u: %u flips, %u depth outliers\n", params.width,
                    params.height, g.flips, g.depthOutliers);
        expectTrue(g.flips <= g.pixels / 500u && g.depthOutliers <= g.hits / 500u, "CUDA tiled march == CPU (tolerance)");
        params.width = 1920;
        params.height = 1080;
        std::vector<f32> depth(1920u * 1080u);
        std::vector<Vec4> normal(1920u * 1080u);
        params.depth_surface = depth.data();
        params.output_surface = normal.data();
        for (const bool useTiles : {false, true}) {
            compute::RayMarchDeviceTiming timing{};
            const bool ran = compute::benchmark_ray_march_cuda(params, useTiles, 10u, timing);
            expectTrue(ran, "CUDA benchmark ran");
            std::printf("SceneData 500-object march 1920x1080 %s: CUDA events kernel min %.3f ms avg %.3f ms "
                        "[60 fps = 16.67 ms]; upload %.3f ms, download %.2f ms; %d regs, %d blocks/SM = %.0f%%\n",
                        useTiles ? "tiled" : "full scene", static_cast<double>(timing.kernel_ms_min),
                        static_cast<double>(timing.kernel_ms_avg), static_cast<double>(timing.upload_ms),
                        static_cast<double>(timing.download_ms), timing.registers_per_thread, timing.blocks_per_sm,
                        100.0 * static_cast<double>(timing.theoretical_occupancy));
            const char* enforce = std::getenv("FUSE_DEVICE_BUDGETS");
            if (useTiles && ran && enforce != nullptr && enforce[0] == '1' && fuse::core::timingBudgetsEnforcedNoted()) {
                expectTrue(timing.kernel_ms_min < 1000.f / 60.f, "500 SDF objects at 1080p > 60 fps (march, CUDA events)");
            }
        }
    }

    scene.destroy();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b3_sdf_scene_500_march: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b3_sdf_scene_500_march: all checks passed\n");
    return EXIT_SUCCESS;
}
