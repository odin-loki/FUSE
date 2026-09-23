#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_job.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::compute::RayMarchParams makeSphereScene() {
    static fuse::compute::SdfObject sphere{};
    sphere.position = {0.f, 0.f, 0.f};
    sphere.params = {1.f, 0.f, 0.f};
    sphere.type = static_cast<fuse::u32>(fuse::compute::SdfPrimitiveType::Sphere);
    sphere.alpha = 0.1f;

    fuse::compute::RayMarchParams params{};
    params.width = 16;
    params.height = 16;
    params.cam_pos = {0.f, 0.f, -3.f};
    params.cam_forward = {0.f, 0.f, 1.f};
    params.cam_right = {1.f, 0.f, 0.f};
    params.cam_up = {0.f, 1.f, 0.f};
    params.fov_rad = 1.0f;
    params.max_steps = 128;
    params.min_dist = 0.001f;
    params.max_dist = 100.f;
    params.objects = &sphere;
    params.object_count = 1;
    return params;
}

void testRayMarcherInfo() {
    const fuse::compute::RayMarcherInfo info = fuse::compute::ray_marcher_info();
    expectTrue(info.valid, "ray marcher info valid");
#if defined(FUSE_HAS_CUDA)
    expectTrue(info.mode == fuse::compute::RayMarcherMode::Cuda, "CUDA backend active");
#else
    expectTrue(info.mode == fuse::compute::RayMarcherMode::CpuReference, "CPU reference mode without toolkit");
    expectTrue(!fuse::jobs::cudaJobsAvailable(), "cuda jobs unavailable without toolkit");
#endif
}

void testCpuSphereHit() {
    const fuse::compute::RayMarchParams params = makeSphereScene();
    const fuse::f32 hitDistance = fuse::compute::ray_march_center_hit_distance(params);
    expectTrue(hitDistance > 0.f, "sphere scene produces a hit");
    expectNear(hitDistance, 2.f, 0.05f, "sphere hit distance near camera-to-surface");
}

fuse::compute::RayMarchParams singleObjectScene(const fuse::compute::SdfObject* object, fuse::math::Vec3 camPos,
                                                fuse::math::Vec3 forward) {
    fuse::compute::RayMarchParams params = makeSphereScene();
    params.objects = object;
    params.object_count = 1;
    params.cam_pos = camPos;
    params.cam_forward = forward;
    return params;
}

void testCpuCapsuleAndTorusHits() {
    using fuse::compute::SdfPrimitiveType;
    // Vertical capsule: radius 0.5, core segment y in [-1, 1].
    fuse::compute::SdfObject capsule{};
    capsule.type = static_cast<fuse::u32>(SdfPrimitiveType::Capsule);
    capsule.params = {0.5f, 1.f, 0.f};
    const fuse::f32 side =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&capsule, {0.f, 0.5f, -3.f}, {0.f, 0.f, 1.f}));
    expectNear(side, 2.5f, 2e-3f, "capsule side hit at the cylinder surface");
    const fuse::f32 cap =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&capsule, {0.f, 5.f, 0.f}, {0.f, -1.f, 0.f}));
    expectNear(cap, 3.5f, 2e-3f, "capsule top hit at the hemispherical cap");
    const fuse::f32 capsuleMiss =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&capsule, {0.7f, 0.f, -3.f}, {0.f, 0.f, 1.f}));
    expectNear(capsuleMiss, -1.f, 0.f, "ray beside the capsule misses");

    // Torus in the XZ plane: major radius 1, tube radius 0.25.
    fuse::compute::SdfObject torus{};
    torus.type = static_cast<fuse::u32>(SdfPrimitiveType::Torus);
    torus.params = {1.f, 0.25f, 0.f};
    const fuse::f32 outer =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&torus, {0.f, 0.f, -3.f}, {0.f, 0.f, 1.f}));
    expectNear(outer, 1.75f, 2e-3f, "torus hit at the outer equator");
    const fuse::f32 tube =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&torus, {1.f, 3.f, 0.f}, {0.f, -1.f, 0.f}));
    expectNear(tube, 2.75f, 2e-3f, "torus hit at the top of the tube");
    const fuse::f32 hole =
        fuse::compute::ray_march_center_hit_distance(singleObjectScene(&torus, {0.f, 3.f, 0.f}, {0.f, -1.f, 0.f}));
    expectNear(hole, -1.f, 0.f, "ray through the torus hole misses");
}

void testHardUnionAlphaZero() {
    // alpha == 0 used to divide by zero in the smooth union (NaN distance, no hit).
    static fuse::compute::SdfObject spheres[2]{};
    spheres[0].type = static_cast<fuse::u32>(fuse::compute::SdfPrimitiveType::Sphere);
    spheres[0].params = {1.f, 0.f, 0.f};
    spheres[0].alpha = 0.f;
    spheres[1] = spheres[0];
    spheres[1].position = {0.f, 0.f, -1.5f};
    spheres[1].params = {0.25f, 0.f, 0.f};
    fuse::compute::RayMarchParams params = makeSphereScene();
    params.objects = spheres;
    params.object_count = 2;
    const fuse::f32 d = fuse::compute::ray_march_scene_distance(params, {0.f, 0.f, -3.f});
    expectTrue(std::isfinite(d), "hard union distance is finite");
    expectNear(d, 1.25f, 1e-5f, "hard union is the minimum distance");
    expectNear(fuse::compute::ray_march_center_hit_distance(params), 1.25f, 2e-3f,
               "hard union hits the nearer sphere");
}

void testFullFrameMatchesAnalyticSphere() {
    fuse::compute::RayMarchParams params = makeSphereScene();
    params.width = 64;
    params.height = 48;
    params.min_dist = 1e-4f;
    params.max_steps = 256;
    std::vector<fuse::f32> depth(params.width * params.height, -2.f);
    std::vector<fuse::math::Vec4> normals(params.width * params.height);
    params.depth_surface = depth.data();
    params.output_surface = normals.data();
    expectTrue(fuse::compute::launch_ray_march_cpu(params), "full-frame CPU ray march succeeds");

    const fuse::f32 tanHalf = std::tan(0.5f * params.fov_rad);
    const fuse::f32 aspect = static_cast<fuse::f32>(params.width) / static_cast<fuse::f32>(params.height);
    fuse::u32 hits = 0;
    fuse::u32 mismatches = 0;
    fuse::f32 worstDepth = 0.f;
    fuse::f32 worstSurface = 0.f;
    fuse::f32 worstNormalDot = 1.f;
    for (fuse::u32 y = 0; y < params.height; ++y) {
        for (fuse::u32 x = 0; x < params.width; ++x) {
            const fuse::f32 ndcX = 2.f * (static_cast<fuse::f32>(x) + 0.5f) / static_cast<fuse::f32>(params.width) - 1.f;
            const fuse::f32 ndcY =
                1.f - 2.f * (static_cast<fuse::f32>(y) + 0.5f) / static_cast<fuse::f32>(params.height);
            const fuse::math::Vec3 d =
                fuse::math::Vec3{ndcX * tanHalf * aspect, ndcY * tanHalf, 1.f}.normalized();
            // Analytic ray / unit sphere at the origin from (0, 0, -3).
            const fuse::math::Vec3 o = params.cam_pos;
            const fuse::f32 b = o.dot(d);
            const fuse::f32 disc = b * b - (o.dot(o) - 1.f);
            const fuse::u32 i = y * params.width + x;
            if (disc < 0.f) {
                if (depth[i] != -1.f || normals[i].w != 0.f) {
                    ++mismatches;
                }
                continue;
            }
            const fuse::f32 t = -b - std::sqrt(disc);
            if (depth[i] < 0.f) {
                // Sphere tracing may run out of steps only on a grazing silhouette ray.
                if (disc > 1e-2f) {
                    ++mismatches;
                }
                continue;
            }
            ++hits;
            const fuse::math::Vec3 n = (o + d * t).normalized();
            // Sphere tracing stops within min_dist of the surface; along the ray that is min_dist / cos(incidence).
            worstSurface = std::max(worstSurface, std::fabs((o + d * depth[i]).length() - 1.f));
            if (-n.dot(d) > 0.2f) {
                worstDepth = std::max(worstDepth, std::fabs(depth[i] - t));
            }
            worstNormalDot = std::min(worstNormalDot, n.dot(fuse::math::Vec3{normals[i].x, normals[i].y, normals[i].z}));
            if (normals[i].w != 1.f) {
                ++mismatches;
            }
        }
    }
    std::printf("[compute] ray march 64x48 sphere: %u hits, %u hit/miss mismatches, hit points within %.2e of the "
                "surface, max |t - analytic| %.2e (incidence cos > 0.2), min normal dot %.6f\n",
                hits, mismatches, worstSurface, worstDepth, worstNormalDot);
    expectTrue(hits > 300u, "sphere covers the expected pixels");
    expectTrue(mismatches == 0u, "full-frame hit/miss matches the analytic sphere");
    expectTrue(worstSurface <= params.min_dist, "full-frame hit points lie within min_dist of the surface");
    expectTrue(worstDepth < 1e-3f, "full-frame depth within 1e-3 of the analytic intersection");
    expectTrue(worstNormalDot > 0.999f, "full-frame normals match the analytic sphere normal");

    fuse::compute::RayMarchParams invalid = params;
    invalid.fov_rad = 0.f;
    expectTrue(!fuse::compute::launch_ray_march_cpu(invalid), "zero FOV rejected");
    invalid = params;
    invalid.objects = nullptr;
    expectTrue(!fuse::compute::launch_ray_march_cpu(invalid), "missing object array rejected");
}

void testLaunchRayMarch() {
    fuse::compute::RayMarchParams params = makeSphereScene();
    std::vector<fuse::f32> depth(params.width * params.height, -2.f);
    params.depth_surface = depth.data();
    expectTrue(fuse::compute::launch_ray_march(params), "launch_ray_march succeeds");
    // CUDA device, or (no toolkit / no device) the CpuParallel fallback: either way every pixel is written.
    expectTrue(std::none_of(depth.begin(), depth.end(), [](fuse::f32 v) { return v == -2.f; }),
               "launch writes every depth pixel");
    const fuse::f32 centre = depth[(params.height / 2) * params.width + params.width / 2];
    expectTrue(centre > 1.9f && centre < 2.1f, "launch depth near the sphere front at the centre");
}

void testSubmitCudaJob() {
    fuse::jobs::JobCounter counter(1);
    bool launcherRan = false;

    fuse::jobs::CUDAJobDesc desc{};
    desc.counter = &counter;
    desc.tag = "test_cuda_job";
    desc.kernel_launcher = [&](fuse::jobs::CUDAStreamHandle stream) {
        launcherRan = true;
#if defined(FUSE_HAS_CUDA)
        // Toolkit compiled in: a managed stream exists only when a CUDA device is present.
        if (fuse::jobs::cudaJobsAvailable()) {
            expectTrue(stream.native != nullptr, "CUDA path receives a stream handle");
        } else {
            expectTrue(stream.native == nullptr, "no CUDA device keeps stream null");
        }
#else
        expectTrue(stream.native == nullptr, "CPU stub passes null stream");
#endif
    };

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    fuse::jobs::submit_cuda(std::move(desc));
    counter.wait();
    scheduler.shutdown();

    expectTrue(launcherRan, "submit_cuda runs kernel launcher");
    expectTrue(counter.isComplete(), "submit_cuda signals job counter");
}

void testSubmitRayMarchJob() {
    fuse::jobs::JobCounter counter(1);
    fuse::compute::RayMarchJobDesc jobDesc{};
    jobDesc.params = makeSphereScene();
    jobDesc.counter = &counter;

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    fuse::compute::submit_ray_march_job(std::move(jobDesc));
    counter.wait();
    scheduler.shutdown();

    expectTrue(counter.isComplete(), "ray march job signals counter");
}

} // namespace

int main() {
    fuse::core::initialize();

    testRayMarcherInfo();
    testCpuSphereHit();
    testCpuCapsuleAndTorusHits();
    testHardUnionAlphaZero();
    testFullFrameMatchesAnalyticSphere();
    testLaunchRayMarch();
    testSubmitCudaJob();
    testSubmitRayMarchJob();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
