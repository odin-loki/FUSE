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

void testBlockerFactorPredicates() {
    expectNear(fuse::audio::clamp_blocker_factor(1.5f), 1.f, 1e-5f,
               "blocker factor clamps above unity");
    expectNear(fuse::audio::clamp_blocker_factor(-0.25f), 0.f, 1e-5f,
               "blocker factor clamps below zero");

    expectTrue(fuse::audio::is_clear_blocker_factor(0.f), "zero blocker factor is clear");
    expectTrue(fuse::audio::is_clear_blocker_factor(-1.f), "negative blocker factor is clear");
    expectTrue(!fuse::audio::is_clear_blocker_factor(0.5f), "mid blocker factor is not clear");

    expectTrue(fuse::audio::is_fully_blocked_blocker_factor(1.f),
               "unity blocker factor is fully blocked");
    expectTrue(fuse::audio::is_fully_blocked_blocker_factor(2.f),
               "excess blocker factor is fully blocked");
    expectTrue(!fuse::audio::is_fully_blocked_blocker_factor(0.75f),
               "partial blocker factor is not fully blocked");
}

void testShouldEvaluateOcclusionBlockers() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};

    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(nullptr, 0,
                                                                fuse::audio::Vec3{},
                                                                fuse::audio::Vec3{1.f, 0.f, 0.f},
                                                                1.f),
               "empty blocker list skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(&blocker, 1,
                                                                fuse::audio::Vec3{},
                                                                fuse::audio::Vec3{},
                                                                1.f),
               "co-located listener and source skip evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(&blocker, 1,
                                                                fuse::audio::Vec3{},
                                                                fuse::audio::Vec3{5.f, 0.f, 0.f},
                                                                0.f),
               "fully occluded source skips blocker evaluation");
    expectTrue(fuse::audio::should_evaluate_occlusion_blockers(&blocker, 1,
                                                               fuse::audio::Vec3{},
                                                               fuse::audio::Vec3{5.f, 0.f, 0.f},
                                                               1.f),
               "valid blockers with separated positions evaluate geometry");

    expectTrue(fuse::audio::should_skip_occlusion_blocker_evaluation(nullptr, 0,
                                                                    fuse::audio::Vec3{},
                                                                    fuse::audio::Vec3{1.f, 0.f, 0.f},
                                                                    1.f),
               "skip predicate is inverse of evaluate predicate for empty list");
    expectTrue(!fuse::audio::should_skip_occlusion_blocker_evaluation(
                   &blocker, 1, fuse::audio::Vec3{}, fuse::audio::Vec3{5.f, 0.f, 0.f}, 1.f),
               "skip predicate is inverse of evaluate predicate for active blockers");
}

void testBlockerEvaluationGuardsPreserveVisibility() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};

    expectNear(fuse::audio::compute_blockers_factor(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                    fuse::audio::Vec3{0.f, 0.f, 0.f}, &blocker, 1),
               0.f, 1e-5f, "co-located source yields zero blocker factor via skip guard");
    expectNear(fuse::audio::compute_effective_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                           fuse::audio::Vec3{0.f, 0.f, 0.f}, 0.f,
                                                           &blocker, 1),
               0.f, 1e-5f, "fully occluded source stays silent even with blockers");
    expectNear(fuse::audio::combine_occlusion_visibility(0.8f, -0.5f), 0.8f, 1e-5f,
               "clear blocker factor early-out preserves visibility");
    expectNear(fuse::audio::combine_occlusion_visibility(0.8f, 1.5f), 0.f, 1e-5f,
               "full blocker factor early-out silences visibility");
}

void testEmptyReverbZoneListPredicate() {
    expectTrue(fuse::audio::is_empty_reverb_zone_list(nullptr, 0),
               "null zone list with zero count is empty");
    expectTrue(fuse::audio::is_empty_reverb_zone_list(nullptr, 3),
               "null zone list with non-zero count is empty");

    const fuse::audio::ReverbZoneParams zones[] = {
        {{{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.5f, 1.f},
    };
    expectTrue(!fuse::audio::is_empty_reverb_zone_list(zones, 1),
               "non-null zone list with count is not empty");
    expectTrue(fuse::audio::is_empty_reverb_zone_list(zones, 0),
               "valid pointer with zero count is empty");
}

void testNearZeroWetMixEpsilon() {
    expectTrue(fuse::audio::is_near_zero_wet_mix(0.f), "zero wet mix is near-zero");
    expectTrue(fuse::audio::is_near_zero_wet_mix(1e-8f), "sub-epsilon wet mix is near-zero");
    expectTrue(!fuse::audio::is_near_zero_wet_mix(1e-5f), "above-epsilon wet mix is not near-zero");
}

void testShouldSkipReverbWetMix() {
    fuse::audio::ReverbZoneBlend dry;
    expectTrue(fuse::audio::should_skip_reverb_wet_mix(dry),
               "default blend skips wet convolution");
    expectTrue(!fuse::audio::should_apply_reverb_wet_mix(dry),
               "skip and apply predicates are complementary for dry blend");

    fuse::audio::ReverbZoneBlend wet;
    wet.wet_dry = 0.5f;
    wet.send_level = 0.8f;
    wet.active_zone_count = 1;
    expectTrue(!fuse::audio::should_skip_reverb_wet_mix(wet),
               "active wet blend does not skip convolution");
    expectTrue(fuse::audio::should_apply_reverb_wet_mix(wet),
               "active wet blend applies convolution");

    fuse::audio::ReverbZoneBlend tiny = wet;
    tiny.wet_dry = 1e-8f;
    expectTrue(fuse::audio::should_skip_reverb_wet_mix(tiny),
               "sub-epsilon effective wet mix skips convolution");

    const fuse::audio::ReverbZoneParams zones[] = {
        {{{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.6f, 0.5f},
    };
    expectTrue(fuse::audio::should_skip_reverb_wet_mix(fuse::audio::Vec3{0.f, 0.f, 0.f}, nullptr, 0),
               "one-shot skip on empty zone list");
    expectTrue(!fuse::audio::should_skip_reverb_wet_mix(fuse::audio::Vec3{0.f, 0.f, 0.f}, zones, 1),
               "one-shot skip is false inside active zone");
}

void testBlendDryWetFromReverbBlend() {
    fuse::audio::ReverbZoneBlend dry;
    expectNear(fuse::audio::blend_dry_wet_from_reverb_blend(dry, 1.f, 0.f), 1.f, 1e-5f,
               "dry blend returns dry sample without wet path");

    fuse::audio::ReverbZoneBlend wet;
    wet.wet_dry = 1.f;
    wet.send_level = 1.f;
    wet.active_zone_count = 1;
    expectNear(fuse::audio::blend_dry_wet_from_reverb_blend(wet, 1.f, 0.f), 0.f, 1e-5f,
               "full wet blend returns wet sample");

    fuse::audio::ReverbZoneBlend half = wet;
    half.wet_dry = 0.5f;
    expectNear(fuse::audio::blend_dry_wet_from_reverb_blend(half, 1.f, 0.f), 0.5f, 1e-5f,
               "mid wet blend linearly mixes dry and wet");
}

} // namespace

int main() {
    fuse::core::initialize();

    testBlockerFactorPredicates();
    testShouldEvaluateOcclusionBlockers();
    testBlockerEvaluationGuardsPreserveVisibility();
    testEmptyReverbZoneListPredicate();
    testNearZeroWetMixEpsilon();
    testShouldSkipReverbWetMix();
    testBlendDryWetFromReverbBlend();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_audio_occlusion_reverb_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_audio_occlusion_reverb_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
