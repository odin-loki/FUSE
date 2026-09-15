#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_effects_job.hpp>
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

fuse::compute::SSAOParams makeSsaoParams() {
    fuse::compute::SSAOParams params{};
    params.width = 16;
    params.height = 16;
    params.strength = 1.5f;
    params.directions = 8;
    params.steps_per_dir = 4;
    return params;
}

fuse::compute::SSRParams makeSsrParams() {
    fuse::compute::SSRParams params{};
    params.width = 16;
    params.height = 16;
    params.max_steps = 64;
    params.use_hiz = true;
    return params;
}

fuse::compute::SSGIParams makeSsgiParams() {
    fuse::compute::SSGIParams params{};
    params.width = 16;
    params.height = 16;
    params.max_bounces = 2;
    params.intensity = 1.f;
    return params;
}

void testScreenSpaceEffectsInfo() {
    const fuse::compute::ScreenSpaceEffectsInfo info = fuse::compute::screen_space_effects_info();
    expectTrue(info.valid, "screen-space effects info valid");
#if defined(FUSE_HAS_CUDA)
    expectTrue(info.mode == fuse::compute::ScreenSpaceEffectsMode::Cuda, "CUDA backend active");
#else
    expectTrue(info.mode == fuse::compute::ScreenSpaceEffectsMode::CpuReference,
               "CPU reference mode without toolkit");
    expectTrue(!fuse::jobs::cudaJobsAvailable(), "cuda jobs unavailable without toolkit");
#endif
}

void testCpuReferenceSamples() {
    const fuse::compute::SSAOParams ssaoParams = makeSsaoParams();
    expectNear(fuse::compute::ssao_center_sample(ssaoParams), 1.f / 1.5f, 0.001f,
               "SSAO CPU reference center sample");

    const fuse::compute::SSRParams ssrParams = makeSsrParams();
    expectNear(fuse::compute::ssr_center_sample(ssrParams), 0.f, 0.001f, "SSR CPU reference center sample");

    const fuse::compute::SSGIParams ssgiParams = makeSsgiParams();
    expectNear(fuse::compute::ssgi_center_sample(ssgiParams), 0.2f, 0.001f,
               "SSGI CPU reference center sample");
}

void testLaunchScreenSpaceEffects() {
    expectTrue(fuse::compute::launch_ssao(makeSsaoParams()), "launch_ssao succeeds");
    expectTrue(fuse::compute::launch_ssr(makeSsrParams()), "launch_ssr succeeds");
    expectTrue(fuse::compute::launch_ssgi(makeSsgiParams()), "launch_ssgi succeeds");
}

void testSubmitScreenSpaceJobs() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    fuse::jobs::JobCounter ssaoCounter(1);
    fuse::compute::SSAOJobDesc ssaoJob{};
    ssaoJob.params = makeSsaoParams();
    ssaoJob.counter = &ssaoCounter;
    fuse::compute::submit_ssao_job(std::move(ssaoJob));
    ssaoCounter.wait();
    expectTrue(ssaoCounter.isComplete(), "SSAO job signals counter");

    fuse::jobs::JobCounter ssrCounter(1);
    fuse::compute::SSRJobDesc ssrJob{};
    ssrJob.params = makeSsrParams();
    ssrJob.counter = &ssrCounter;
    fuse::compute::submit_ssr_job(std::move(ssrJob));
    ssrCounter.wait();
    expectTrue(ssrCounter.isComplete(), "SSR job signals counter");

    fuse::jobs::JobCounter ssgiCounter(1);
    fuse::compute::SSGIJobDesc ssgiJob{};
    ssgiJob.params = makeSsgiParams();
    ssgiJob.counter = &ssgiCounter;
    fuse::compute::submit_ssgi_job(std::move(ssgiJob));
    ssgiCounter.wait();
    expectTrue(ssgiCounter.isComplete(), "SSGI job signals counter");

    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();

    testScreenSpaceEffectsInfo();
    testCpuReferenceSamples();
    testLaunchScreenSpaceEffects();
    testSubmitScreenSpaceJobs();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
