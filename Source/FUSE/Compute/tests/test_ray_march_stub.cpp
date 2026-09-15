#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_job.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void testLaunchRayMarch() {
    const fuse::compute::RayMarchParams params = makeSphereScene();
    expectTrue(fuse::compute::launch_ray_march(params), "launch_ray_march succeeds");
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
        expectTrue(stream.native != nullptr, "CUDA path receives a stream handle");
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
    testLaunchRayMarch();
    testSubmitCudaJob();
    testSubmitRayMarchJob();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
