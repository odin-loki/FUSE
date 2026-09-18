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

    expectTrue(fuse::audio::is_empty_occlusion_blockers(nullptr, 0),
               "null blocker list is empty");
    expectTrue(fuse::audio::is_empty_occlusion_blockers(&blocker, 0),
               "zero blocker count is empty");
    expectTrue(!fuse::audio::is_empty_occlusion_blockers(&blocker, 1),
               "valid blocker list is non-empty");

    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, source, nullptr, 0),
               "null blocker list skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, source, &blocker, 0),
               "zero blocker count skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(listener, listener, &blocker, 1),
               "co-located positions skip evaluation");
    expectTrue(fuse::audio::should_evaluate_occlusion_blockers(listener, source, &blocker, 1),
               "separated positions with blockers evaluate");

    expectTrue(fuse::audio::should_skip_blockers_visibility(listener, source, nullptr, 0),
               "empty blocker list skips visibility evaluation");
    expectTrue(fuse::audio::should_skip_blockers_visibility(listener, listener, &blocker, 1),
               "co-located positions skip visibility evaluation");
    expectTrue(!fuse::audio::should_skip_blockers_visibility(listener, source, &blocker, 1),
               "separated positions with blockers evaluate visibility");
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

void testSkipOcclusionBlockerAttenuation() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const fuse::audio::Vec3 listener{0.f, 0.f, 0.f};
    const fuse::audio::Vec3 source{10.f, 0.f, 0.f};

    expectTrue(
        fuse::audio::should_skip_occlusion_blocker_attenuation(listener, source, nullptr, 0),
        "empty blocker list skips blocker attenuation pipeline");
    expectTrue(
        fuse::audio::should_skip_occlusion_blocker_attenuation(listener, listener, &blocker, 1),
        "co-located positions skip blocker attenuation pipeline");
    expectTrue(
        !fuse::audio::should_skip_occlusion_blocker_attenuation(listener, source, &blocker, 1),
        "separated positions with blockers run blocker attenuation pipeline");

    const fuse::audio::OcclusionAttenuation empty_blockers =
        fuse::audio::evaluate_occlusion_from_blockers(listener, source, 1.f, nullptr, 0);
    expectNear(empty_blockers.gain, 1.f, 1e-5f,
               "empty blocker list preserves unity LF via attenuation bypass");
    expectNear(empty_blockers.hf_gain, 1.f, 1e-5f,
               "empty blocker list preserves unity HF via attenuation bypass");

    const fuse::audio::OcclusionAttenuation colocated =
        fuse::audio::evaluate_occlusion_from_blockers(listener, listener, 0.5f, &blocker, 1);
    expectNear(colocated.gain, fuse::audio::evaluate_occlusion_gain(0.5f), 1e-5f,
               "co-located bypass maps source occlusion without blocker factor");
}

void testHasReverbZonesGuard() {
    const fuse::audio::ReverbZoneParams zone{
        {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.5f, 0.8f};

    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 0), "null zone list is empty");
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 2), "null pointer with count is empty");
    expectTrue(!fuse::audio::has_reverb_zones(&zone, 0), "zero zone count is empty");
    expectTrue(fuse::audio::has_reverb_zones(&zone, 1), "valid zone list is non-empty");

    expectTrue(fuse::audio::is_empty_reverb_zones(nullptr, 0), "is_empty matches null list");
    expectTrue(fuse::audio::is_empty_reverb_zones(&zone, 0), "is_empty matches zero count");
    expectTrue(!fuse::audio::is_empty_reverb_zones(&zone, 1), "is_empty false for valid list");
    expectTrue(fuse::audio::should_skip_reverb_zone_blend(nullptr, 0),
               "should_skip_reverb_zone_blend on empty list");
    expectTrue(fuse::audio::should_skip_reverb_zone_blend(&zone, 0),
               "should_skip_reverb_zone_blend on zero count");
    expectTrue(!fuse::audio::should_skip_reverb_zone_blend(&zone, 1),
               "should_skip_reverb_zone_blend false for valid list");
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
    expectTrue(!fuse::audio::should_skip_wet_mix_processing(blend),
               "active zone blend does not skip wet processing");
    expectNear(wet_mix, 0.2f, 1e-5f, "effective wet mix is wet_dry times send_level");
    expectNear(send_gain, 0.5f, 1e-5f, "effective send gain matches clamped send_level");
    expectNear(fuse::audio::compute_effective_wet_mix(listener, zones, 1), wet_mix, 1e-5f,
               "one-shot wet mix matches blend path");
    expectNear(fuse::audio::compute_effective_wet_mix(listener, nullptr, 0), 0.f, 1e-5f,
               "one-shot wet mix on empty list is zero");

    fuse::audio::ReverbZoneBlend dry;
    expectTrue(fuse::audio::should_skip_wet_mix_processing(dry),
               "default blend skips wet processing");
    expectTrue(fuse::audio::is_dry_reverb_blend(dry),
               "should_skip_wet_mix matches is_dry_reverb_blend for inactive blend");

    fuse::audio::ReverbZoneBlend zero_wet = blend;
    zero_wet.wet_dry = 0.f;
    expectTrue(fuse::audio::should_skip_wet_mix_processing(zero_wet),
               "zero wet_dry skips wet processing");
}

} // namespace

int main() {
    fuse::core::initialize();
    testShouldEvaluateOcclusionBlockers();
    testShouldSkipOcclusionAttenuation();
    testOcclusionCombinedGainHelpers();
    testFullyOccludedVisibilityEarlyOut();
    testSkipOcclusionBlockerAttenuation();
    testHasReverbZonesGuard();
    testEffectiveSendGain();
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
