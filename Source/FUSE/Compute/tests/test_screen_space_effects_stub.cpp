#include <fuse/compute/screen_space_contact.hpp>
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
    params.enable_blur = true;
    params.blur_depth_threshold = 0.001f;
    params.blur_normal_threshold = 0.95f;
    params.contact_depth_scale = 0.05f;
    params.contact_normal_power = 2.f;
    return params;
}

fuse::compute::SSRParams makeSsrParams() {
    fuse::compute::SSRParams params{};
    params.width = 16;
    params.height = 16;
    params.max_steps = 64;
    params.use_hiz = true;
    params.contact_hardening = true;
    params.contact_distance = 0.5f;
    params.contact_roughness_floor = 0.02f;
    params.contact_harden_exponent = 2.f;
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
    expectNear(fuse::compute::ssr_center_sample(ssrParams), 0.5f, 0.001f,
               "SSR CPU reference center sample reflects edge fade and roughness");

    const fuse::compute::SSGIParams ssgiParams = makeSsgiParams();
    expectNear(fuse::compute::ssgi_center_sample(ssgiParams), 0.2f, 0.001f,
               "SSGI CPU reference center sample");
}

void testSsaoContactWeighting() {
    const fuse::compute::SSAOParams params = makeSsaoParams();

    expectNear(fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params), 1.f, 0.001f,
               "aligned contact yields full AO weight");
    expectTrue(fuse::compute::ssao_contact_ao_weight(params.contact_depth_scale, 1.f, params) <
                   fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params),
               "large depth delta reduces contact AO weight");
    expectTrue(fuse::compute::ssao_contact_ao_weight(0.f, 0.f, params) <
                   fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params),
               "dissimilar normals reduce contact AO weight");
}

void testSsaoBlurWeight() {
    const fuse::compute::SSAOParams params = makeSsaoParams();

    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 1.f, params), 1.f, 0.001f,
               "identical depth/normal yields full blur weight");
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.01f, 1.f, 1.f, params), 0.f, 0.001f,
               "large depth delta rejects blur tap");
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 0.5f, params), 0.f, 0.001f,
               "dissimilar normals reject blur tap");

    fuse::compute::SSAOParams disabledBlur = params;
    disabledBlur.enable_blur = false;
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 1.f, disabledBlur), 0.f, 0.001f,
               "disabled blur yields zero weight");
}

void testSsrContactHardening() {
    const fuse::compute::SSRParams params = makeSsrParams();

    expectNear(fuse::compute::ssr_contact_harden_roughness(0.f, 0.8f, params), 0.02f, 0.001f,
               "contact hit snaps roughness to floor");
    expectNear(fuse::compute::ssr_contact_harden_roughness(params.contact_distance, 0.8f, params), 0.8f,
               0.001f, "distant hit preserves material roughness");

    fuse::compute::SSRParams disabled = params;
    disabled.contact_hardening = false;
    expectNear(fuse::compute::ssr_contact_harden_roughness(0.f, 0.8f, disabled), 0.8f, 0.001f,
               "disabled contact hardening preserves roughness");
}

void testSsrScreenEdgeFade() {
    const fuse::compute::SSRParams params = makeSsrParams();

    expectNear(fuse::compute::ssr_screen_edge_fade(0.5f, 0.5f, params), 1.f, 0.001f,
               "center pixel has full edge fade");
    expectNear(fuse::compute::ssr_screen_edge_fade(0.f, 0.5f, params), 0.f, 0.001f,
               "screen border fades to zero");

    fuse::compute::SSRParams noFade = params;
    noFade.fade_screen_edge = 0.f;
    expectNear(fuse::compute::ssr_screen_edge_fade(0.f, 0.f, noFade), 1.f, 0.001f,
               "zero fade width disables edge attenuation");
}

void testParamValidation() {
    expectTrue(fuse::compute::validate_ssao_params(makeSsaoParams()), "default SSAO params valid");

    fuse::compute::SSAOParams invalidSsao = makeSsaoParams();
    invalidSsao.directions = 0;
    expectTrue(!fuse::compute::validate_ssao_params(invalidSsao), "zero SSAO directions rejected");

    invalidSsao = makeSsaoParams();
    invalidSsao.blur_normal_threshold = 1.5f;
    expectTrue(!fuse::compute::validate_ssao_params(invalidSsao), "SSAO blur normal threshold clamped");

    expectTrue(fuse::compute::validate_ssr_params(makeSsrParams()), "default SSR params valid");

    fuse::compute::SSRParams invalidSsr = makeSsrParams();
    invalidSsr.contact_distance = 0.f;
    expectTrue(!fuse::compute::validate_ssr_params(invalidSsr), "zero SSR contact distance rejected");

    invalidSsr = makeSsrParams();
    invalidSsr.max_steps = 0;
    expectTrue(!fuse::compute::validate_ssr_params(invalidSsr), "zero SSR max steps rejected");
}

void testLaunchScreenSpaceEffects() {
    expectTrue(fuse::compute::launch_ssao(makeSsaoParams()), "launch_ssao succeeds");
    expectTrue(fuse::compute::launch_ssr(makeSsrParams()), "launch_ssr succeeds");
    expectTrue(fuse::compute::launch_ssgi(makeSsgiParams()), "launch_ssgi succeeds");

    fuse::compute::SSAOParams invalidSsao = makeSsaoParams();
    invalidSsao.width = 0;
    expectTrue(!fuse::compute::launch_ssao(invalidSsao), "launch_ssao rejects invalid params");

    fuse::compute::SSRParams invalidSsr = makeSsrParams();
    invalidSsr.ray_step_size = 0.f;
    expectTrue(!fuse::compute::launch_ssr(invalidSsr), "launch_ssr rejects invalid params");
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
    testSsaoContactWeighting();
    testSsaoBlurWeight();
    testSsrContactHardening();
    testSsrScreenEdgeFade();
    testParamValidation();
    testLaunchScreenSpaceEffects();
    testSubmitScreenSpaceJobs();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
