#include <fuse/audio/occlusion.hpp>
#include <fuse/audio/reverb_zones.hpp>
#include <fuse/audio/spatial_mixer.hpp>
#include <fuse/core/init.hpp>

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

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testOcclusionBlockerEvalSkip() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};

    expectTrue(fuse::audio::should_skip_occlusion_blocker_eval(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                             fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                             nullptr, 0, 1.f),
               "no blockers skip occlusion blocker evaluation");
    expectTrue(fuse::audio::should_skip_occlusion_blocker_eval(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                             fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                             &blocker, 1, 1.f),
               "co-located listener and source skip blocker evaluation");
    expectTrue(fuse::audio::should_skip_occlusion_blocker_eval(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                             fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                             &blocker, 1, 0.f),
               "fully occluded source skips blocker evaluation");
    expectTrue(!fuse::audio::should_skip_occlusion_blocker_eval(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                              fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                              &blocker, 1, 0.5f),
               "partial occlusion with blockers evaluates geometry");
}

void testOcclusionCombinedGain() {
    const fuse::audio::OcclusionAttenuation unity =
        fuse::audio::make_unity_occlusion_attenuation();
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(unity),
               "unity factory produces unity attenuation");
    expectNear(fuse::audio::compute_occlusion_combined_gain(unity), 1.f, 1e-5f,
               "unity combined gain is one");

    fuse::audio::OcclusionAttenuation partial;
    partial.gain = 0.5f;
    partial.hf_gain = 0.8f;
    expectNear(fuse::audio::compute_occlusion_combined_gain(partial), 0.4f, 1e-5f,
               "combined gain multiplies LF and HF");

    const fuse::audio::OcclusionAttenuation from_visibility =
        fuse::audio::evaluate_occlusion_attenuation(1.f);
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(from_visibility),
               "full visibility evaluates to unity attenuation");
    expectNear(fuse::audio::compute_occlusion_combined_gain(from_visibility), 1.f, 1e-5f,
               "full visibility combined gain is unity");
}

void testSpatialMixerOcclusionGuards() {
    fuse::audio::SpatialMixer mixer;
    expectTrue(!mixer.has_occlusion_blockers(), "mixer starts without blockers");

    const fuse::audio::OcclusionAttenuation clear =
        mixer.compute_source_occlusion_attenuation(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                   fuse::audio::Vec3{10.f, 0.f, 0.f}, 1.f);
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(clear),
               "mixer returns unity attenuation without blockers");
    expectNear(fuse::audio::compute_occlusion_combined_gain(clear), 1.f, 1e-5f,
               "mixer unity path preserves combined gain");

    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    mixer.set_occlusion_blockers(&blocker, 1);
    expectTrue(mixer.has_occlusion_blockers(), "mixer reports registered blockers");
    expectTrue(!fuse::audio::is_unity_occlusion_attenuation(
                   mixer.compute_source_occlusion_attenuation(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                              fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                              1.f)),
               "blocker pipeline attenuates occlusion");
    mixer.clear_occlusion_blockers();
    expectTrue(!mixer.has_occlusion_blockers(), "clear removes blockers");
}

void testReverbZoneListGuard() {
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 0), "null zone list is empty");
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 2), "null pointer with count is empty");

    const fuse::audio::ReverbZoneParams zone{};
    expectTrue(fuse::audio::has_reverb_zones(&zone, 1), "non-null zone list is non-empty");
    expectTrue(!fuse::audio::has_reverb_zones(&zone, 0), "zero zone count is empty");
}

void testNearZeroWetMix() {
    expectTrue(fuse::audio::is_near_zero_wet_mix(0.f), "zero wet mix is near-zero");
    expectTrue(fuse::audio::is_near_zero_wet_mix(1e-8f), "sub-epsilon wet mix is near-zero");
    expectTrue(!fuse::audio::is_near_zero_wet_mix(0.01f), "audible wet mix is not near-zero");
    expectTrue(fuse::audio::is_near_zero_wet_mix(-1.f), "negative wet mix clamps to near-zero");
}

void testBlendReverbSampleEarlyOuts() {
    fuse::audio::ReverbZoneBlend dry;
    expectNear(fuse::audio::blend_reverb_sample(0.8f, 0.2f, dry), 0.8f, 1e-5f,
               "dry blend early-out returns dry sample");

    fuse::audio::ReverbZoneBlend zero_send;
    zero_send.wet_dry = 0.6f;
    zero_send.send_level = 0.f;
    zero_send.active_zone_count = 1;
    expectTrue(fuse::audio::should_skip_reverb_wet_mix(zero_send),
               "zero send skips wet mix");
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, zero_send), 1.f, 1e-5f,
               "zero send early-out returns dry sample");

    fuse::audio::ReverbZoneBlend wet;
    wet.wet_dry = 1.f;
    wet.send_level = 1.f;
    wet.active_zone_count = 1;
    expectTrue(!fuse::audio::should_skip_reverb_wet_mix(wet),
               "full wet blend applies reverb");
    expectNear(fuse::audio::blend_reverb_sample(0.f, 1.f, wet), 1.f, 1e-5f,
               "full wet blend returns wet sample");
    expectNear(fuse::audio::blend_reverb_sample(0.8f, 0.2f, wet), 0.2f, 1e-5f,
               "full wet blend matches wet scalar");

    fuse::audio::ReverbZoneBlend half;
    half.wet_dry = 0.5f;
    half.send_level = 1.f;
    half.active_zone_count = 1;
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, half), 0.5f, 1e-5f,
               "mid wet blend linearly mixes dry and wet");
}

} // namespace

int main() {
    fuse::core::initialize();

    testOcclusionBlockerEvalSkip();
    testOcclusionCombinedGain();
    testSpatialMixerOcclusionGuards();
    testReverbZoneListGuard();
    testNearZeroWetMix();
    testBlendReverbSampleEarlyOuts();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All fuse_audio_occlusion_reverb tests passed.\n");
    return EXIT_SUCCESS;
}
