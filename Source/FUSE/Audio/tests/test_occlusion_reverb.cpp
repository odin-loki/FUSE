#include <fuse/audio/occlusion.hpp>
#include <fuse/audio/reverb_zones.hpp>
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

void testShouldEvaluateOcclusionBlockers() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const fuse::audio::Vec3 listener{0.f, 0.f, 0.f};
    const fuse::audio::Vec3 source{10.f, 0.f, 0.f};

    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, source, nullptr, 0),
               "null blocker list skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, source, &blocker, 0),
               "zero blocker count skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, listener, &blocker, 1),
               "co-located positions skip evaluation");
    expectTrue(fuse::audio::should_evaluate_occlusion_blockers(listener, source, &blocker, 1),
               "separated positions with blockers evaluate");
}

void testShouldSkipOcclusionAttenuation() {
    expectTrue(fuse::audio::should_skip_occlusion_attenuation(1.f),
               "unity visibility skips attenuation mapping");
    expectTrue(fuse::audio::should_skip_occlusion_attenuation(1.5f),
               "above-unity visibility skips attenuation mapping");
    expectTrue(!fuse::audio::should_skip_occlusion_attenuation(0.5f),
               "partial visibility requires attenuation mapping");
    expectTrue(!fuse::audio::should_skip_occlusion_attenuation(0.f),
               "fully occluded visibility does not skip attenuation mapping");
}

void testOcclusionCombinedGainHelpers() {
    const fuse::audio::OcclusionAttenuation unity{1.f, 1.f};
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(unity),
               "unity attenuation is recognised");
    expectNear(fuse::audio::compute_occlusion_combined_gain(unity), 1.f, 1e-5f,
               "unity combined gain is one");

    const fuse::audio::OcclusionAttenuation blocked{0.1f, 0.6f};
    expectTrue(!fuse::audio::is_unity_occlusion_attenuation(blocked),
               "blocked attenuation is not unity");
    expectNear(fuse::audio::compute_occlusion_combined_gain(blocked), 0.06f, 1e-5f,
               "combined gain multiplies LF and HF");

    const fuse::audio::OcclusionAttenuation from_visibility =
        fuse::audio::evaluate_occlusion_attenuation(1.2f);
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(from_visibility),
               "fully visible early-out yields unity attenuation");
    expectTrue(fuse::audio::should_skip_occlusion_attenuation(1.2f),
               "should_skip matches evaluate early-out path");
}

void testFullyOccludedVisibilityEarlyOut() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectNear(fuse::audio::compute_effective_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                         fuse::audio::Vec3{10.f, 0.f, 0.f}, 0.f,
                                                         &blocker, 1),
               0.f, 1e-5f, "fully occluded source early-outs before blocker evaluation");
    expectNear(fuse::audio::compute_blockers_factor(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                    fuse::audio::Vec3{0.f, 0.f, 0.f}, &blocker, 1),
               0.f, 1e-5f, "co-located source yields zero blocker factor via guard");
}

void testHasReverbZonesGuard() {
    const fuse::audio::ReverbZoneParams zone{
        {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.5f, 0.8f};

    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 0), "null zone list is empty");
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 2), "null pointer with count is empty");
    expectTrue(!fuse::audio::has_reverb_zones(&zone, 0), "zero zone count is empty");
    expectTrue(fuse::audio::has_reverb_zones(&zone, 1), "valid zone list is non-empty");
}

void testEffectiveSendGain() {
    fuse::audio::ReverbZoneBlend dry;
    expectNear(fuse::audio::compute_effective_send_gain(dry), 0.f, 1e-5f,
               "inactive blend yields zero send gain");

    fuse::audio::ReverbZoneBlend wet;
    wet.wet_dry = 0.6f;
    wet.send_level = 0.75f;
    wet.active_zone_count = 1;
    expectNear(fuse::audio::compute_effective_send_gain(wet), 0.75f, 1e-5f,
               "active blend exposes clamped send level");

    wet.send_level = 1.5f;
    expectNear(fuse::audio::compute_effective_send_gain(wet), 1.f, 1e-5f,
               "send level above unity clamps to one");
}

void testBlendReverbSampleOneShot() {
    const fuse::audio::ReverbZoneParams zone{
        {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.5f, 1.f};
    const fuse::audio::Vec3 inside{0.f, 0.f, 0.f};
    const fuse::audio::Vec3 outside{100.f, 0.f, 0.f};

    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, inside, nullptr, 0), 1.f, 1e-5f,
               "empty zone list returns dry sample");
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, outside, &zone, 1), 1.f, 1e-5f,
               "listener outside zones returns dry sample");
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, inside, &zone, 1), 0.5f, 1e-5f,
               "listener inside zone blends dry and wet");

    const fuse::audio::ReverbZoneParams dry_zone{
        {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.f, 1.f};
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.5f, inside, &dry_zone, 1), 1.f, 1e-5f,
               "dry zone inside bounds still returns dry sample");
}

void testEmptyOcclusionBlockerGuards() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const fuse::audio::Vec3 listener{0.f, 0.f, 0.f};
    const fuse::audio::Vec3 source{10.f, 0.f, 0.f};

    expectTrue(fuse::audio::has_empty_occlusion_blockers(nullptr, 0),
               "null blocker list is empty");
    expectTrue(fuse::audio::has_empty_occlusion_blockers(nullptr, 2),
               "null pointer with count is empty");
    expectTrue(fuse::audio::has_empty_occlusion_blockers(&blocker, 0),
               "zero blocker count is empty");
    expectTrue(!fuse::audio::has_empty_occlusion_blockers(&blocker, 1),
               "valid blocker list is non-empty");

    expectTrue(fuse::audio::should_skip_occlusion_from_blockers(listener, source, 1.f, nullptr, 0),
               "empty blocker list skips blocker attenuation pipeline");
    expectTrue(fuse::audio::should_skip_occlusion_from_blockers(listener, source, 0.f, &blocker, 1),
               "fully occluded source skips blocker attenuation pipeline");
    expectTrue(fuse::audio::should_skip_occlusion_from_blockers(listener, listener, 0.5f, &blocker, 1),
               "co-located positions skip blocker attenuation pipeline");
    expectTrue(!fuse::audio::should_skip_occlusion_from_blockers(listener, source, 1.f, &blocker, 1),
               "fully visible source with blockers still evaluates geometry");

    const fuse::audio::OcclusionAttenuation empty_blockers =
        fuse::audio::evaluate_occlusion_from_blockers(listener, source, 1.f, nullptr, 0);
    expectTrue(fuse::audio::is_unity_occlusion_attenuation(empty_blockers),
               "empty blocker early-out yields unity attenuation");
}

void testCombineOcclusionVisibilityGuards() {
    expectTrue(fuse::audio::should_skip_combine_occlusion_visibility(0.8f, 0.f),
               "zero blocker factor skips combine");
    expectTrue(fuse::audio::should_skip_combine_occlusion_visibility(0.f, 0.5f),
               "fully occluded source skips combine");
    expectTrue(!fuse::audio::should_skip_combine_occlusion_visibility(0.8f, 0.5f),
               "partial visibility and blocker factor combine");

    expectNear(fuse::audio::combine_occlusion_visibility(0.8f, 0.f), 0.8f, 1e-5f,
               "skip guard preserves source visibility");
    expectNear(fuse::audio::combine_occlusion_visibility(0.f, 0.75f), 0.f, 1e-5f,
               "skip guard silences fully occluded source");
}

void testDryWetBlendGuards() {
    expectTrue(fuse::audio::is_zero_wet_mix(0.f), "zero wet mix is zero");
    expectTrue(fuse::audio::is_zero_wet_mix(-0.5f), "negative wet mix clamps to zero");
    expectTrue(fuse::audio::is_full_wet_mix(1.f), "unity wet mix is full");
    expectTrue(fuse::audio::is_full_wet_mix(1.5f), "above-unity wet mix clamps to full");
    expectTrue(fuse::audio::should_skip_dry_wet_blend(0.f), "zero wet mix skips blend");
    expectTrue(fuse::audio::should_skip_dry_wet_blend(1.f), "unity wet mix skips blend");
    expectTrue(!fuse::audio::should_skip_dry_wet_blend(0.5f), "partial wet mix blends");

    expectNear(fuse::audio::blend_dry_wet_sample(1.f, 0.f, 0.f), 1.f, 1e-5f,
               "zero wet mix guard returns dry");
    expectNear(fuse::audio::blend_dry_wet_sample(1.f, 0.f, 1.f), 0.f, 1e-5f,
               "unity wet mix guard returns wet");
}

void testReverbZoneBlendEarlyOuts() {
    const fuse::audio::ReverbZoneParams zone{
        {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.5f, 1.f};
    const fuse::audio::Vec3 inside{0.f, 0.f, 0.f};

    expectTrue(fuse::audio::should_skip_reverb_zone_blend(nullptr, 0),
               "null zone list skips blend");
    expectTrue(fuse::audio::should_skip_reverb_zone_blend(&zone, 0),
               "zero zone count skips blend");
    expectTrue(!fuse::audio::should_skip_reverb_zone_blend(&zone, 1),
               "valid zone list does not skip blend");

    fuse::audio::ReverbZoneBlend dry;
    expectTrue(fuse::audio::should_skip_reverb_wet_convolution(dry),
               "inactive blend skips wet convolution");
    expectTrue(!fuse::audio::should_apply_reverb_wet_mix(dry),
               "skip guard is inverse of apply guard");

    fuse::audio::ReverbZoneBlend wet;
    wet.wet_dry = 0.4f;
    wet.send_level = 0.5f;
    wet.active_zone_count = 1;
    expectTrue(!fuse::audio::should_skip_reverb_wet_convolution(wet),
               "active wet blend does not skip convolution");
    expectNear(fuse::audio::blend_reverb_from_blend(1.f, 0.f, wet), 0.8f, 1e-5f,
               "blend helper matches wet mix path");
    expectNear(fuse::audio::blend_reverb_from_blend(1.f, 0.f, dry), 1.f, 1e-5f,
               "inactive blend helper returns dry sample");
    expectNear(fuse::audio::blend_reverb_sample(1.f, 0.f, inside, &zone, 1), 0.5f, 1e-5f,
               "one-shot sample matches zone blend path");
}

void testWetMixGuardConsistency() {
    const fuse::audio::ReverbZoneParams zones[] = {
        {{{-10.f, -10.f, -10.f}, {10.f, 10.f, 10.f}}, 0.4f, 0.5f},
    };
    const fuse::audio::Vec3 listener{0.f, 0.f, 0.f};

    const fuse::audio::ReverbZoneBlend blend =
        fuse::audio::blend_reverb_zones(listener, zones, 1);
    const float wet_mix = fuse::audio::compute_effective_wet_mix(blend);
    const float send_gain = fuse::audio::compute_effective_send_gain(blend);

    expectTrue(fuse::audio::should_apply_reverb_wet_mix(blend),
               "active zone blend applies wet mix");
    expectNear(wet_mix, 0.2f, 1e-5f, "effective wet mix is wet_dry times send_level");
    expectNear(send_gain, 0.5f, 1e-5f, "effective send gain matches clamped send_level");
    expectNear(fuse::audio::compute_effective_wet_mix(listener, zones, 1), wet_mix, 1e-5f,
               "one-shot wet mix matches blend path");
    expectNear(fuse::audio::compute_effective_wet_mix(listener, nullptr, 0), 0.f, 1e-5f,
               "one-shot wet mix on empty list is zero");
}

} // namespace

int main() {
    fuse::core::initialize();
    testShouldEvaluateOcclusionBlockers();
    testShouldSkipOcclusionAttenuation();
    testOcclusionCombinedGainHelpers();
    testFullyOccludedVisibilityEarlyOut();
    testEmptyOcclusionBlockerGuards();
    testCombineOcclusionVisibilityGuards();
    testHasReverbZonesGuard();
    testEffectiveSendGain();
    testDryWetBlendGuards();
    testReverbZoneBlendEarlyOuts();
    testBlendReverbSampleOneShot();
    testWetMixGuardConsistency();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("test_occlusion_reverb: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "test_occlusion_reverb: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
