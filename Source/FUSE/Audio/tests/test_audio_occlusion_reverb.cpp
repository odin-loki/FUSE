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

void testShouldEvaluateOcclusionBlockers() {
    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};

    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                                fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                                nullptr, 0, 1.f),
               "null blocker list skips evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                                fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                                &blocker, 1, 0.f),
               "fully occluded source skips blocker evaluation");
    expectTrue(!fuse::audio::should_evaluate_occlusion_blockers(fuse::audio::Vec3{1.f, 2.f, 3.f},
                                                                fuse::audio::Vec3{1.f, 2.f, 3.f},
                                                                &blocker, 1, 1.f),
               "co-located listener and source skip blocker evaluation");
    expectTrue(fuse::audio::should_evaluate_occlusion_blockers(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                               fuse::audio::Vec3{10.f, 0.f, 0.f},
                                                               &blocker, 1, 0.5f),
               "separated source with blockers evaluates blockers");
}

void testBlockerFactorPredicates() {
    expectTrue(fuse::audio::is_clear_blocker_factor(0.f), "zero blocker factor is clear");
    expectTrue(fuse::audio::is_clear_blocker_factor(-0.5f), "negative blocker factor clamps clear");
    expectTrue(!fuse::audio::is_clear_blocker_factor(0.25f), "partial blocker factor is not clear");

    expectTrue(fuse::audio::is_full_blocker_factor(1.f), "unity blocker factor is full");
    expectTrue(fuse::audio::is_full_blocker_factor(1.5f), "above-unity blocker factor clamps full");
    expectTrue(!fuse::audio::is_full_blocker_factor(0.75f), "partial blocker factor is not full");
}

void testHasReverbZonesPredicate() {
    const fuse::audio::ReverbZoneParams zone{};
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 0), "null zone list is empty");
    expectTrue(!fuse::audio::has_reverb_zones(nullptr, 2), "null pointer with count is empty");
    expectTrue(!fuse::audio::has_reverb_zones(&zone, 0), "zero zone count is empty");
    expectTrue(fuse::audio::has_reverb_zones(&zone, 1), "non-null zone list is non-empty");
}

void testComputeListenerReverbBlendGuard() {
    const fuse::audio::ReverbZoneParams zones[] = {
        {{{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}}, 0.6f, 0.5f},
    };

    const fuse::audio::ReverbZoneBlend empty =
        fuse::audio::compute_listener_reverb_blend(fuse::audio::Vec3{0.f, 0.f, 0.f}, nullptr, 0);
    expectTrue(empty.active_zone_count == 0, "empty zone list yields dry blend");
    expectTrue(fuse::audio::should_skip_reverb_wet_mix(empty),
               "empty zone list skips wet convolution");

    const fuse::audio::ReverbZoneBlend inside =
        fuse::audio::compute_listener_reverb_blend(fuse::audio::Vec3{0.f, 0.f, 0.f}, zones, 1);
    expectNear(inside.wet_dry, 0.6f, 1e-5f, "listener blend preserves zone wet_dry");
    expectTrue(fuse::audio::should_apply_reverb_wet_mix(inside),
               "active zone blend applies wet convolution");
    expectTrue(!fuse::audio::should_skip_reverb_wet_mix(inside),
               "active zone blend does not skip wet convolution");
}

void testWetMixEpsilonPredicates() {
    expectTrue(fuse::audio::is_near_zero_wet_mix(0.f), "zero wet mix is near-zero");
    expectTrue(fuse::audio::is_near_zero_wet_mix(1e-7f), "sub-epsilon wet mix is near-zero");
    expectTrue(!fuse::audio::is_near_zero_wet_mix(1e-4f), "above-epsilon wet mix is non-zero");

    expectTrue(fuse::audio::is_unity_wet_mix(1.f), "unity wet mix is unity");
    expectTrue(fuse::audio::is_unity_wet_mix(1.f - 1e-7f), "near-unity wet mix is unity");
    expectTrue(!fuse::audio::is_unity_wet_mix(0.5f), "mid-range wet mix is not unity");

    expectNear(fuse::audio::blend_dry_wet_sample(1.f, 0.f, 1e-7f), 1.f, 1e-5f,
               "sub-epsilon wet mix early-out returns dry sample");
    expectNear(fuse::audio::blend_dry_wet_sample(1.f, 0.f, 1.f - 1e-7f), 0.f, 1e-5f,
               "near-unity wet mix early-out returns wet sample");
}

void testSpatialMixerOcclusionBlockerGuard() {
    fuse::audio::SpatialMixer mixer;
    expectTrue(!mixer.has_occlusion_blockers(), "default mixer has no blockers");

    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    mixer.set_occlusion_blockers(&blocker, 1);
    expectTrue(mixer.has_occlusion_blockers(), "registered blocker list is non-empty");

    mixer.set_occlusion_blockers(nullptr, 0);
    expectTrue(!mixer.has_occlusion_blockers(), "null blocker registration clears blockers");

    mixer.set_occlusion_blockers(&blocker, 1);
    expectNear(mixer.compute_source_visibility(fuse::audio::Vec3{1.f, 2.f, 3.f},
                                               fuse::audio::Vec3{1.f, 2.f, 3.f}, 0.8f),
               0.8f, 1e-5f, "co-located source preserves visibility with registered blockers");

    mixer.clear_occlusion_blockers();
    expectTrue(!mixer.has_occlusion_blockers(), "clearing blockers empties mixer list");
}

} // namespace

int main() {
    fuse::core::initialize();

    testShouldEvaluateOcclusionBlockers();
    testBlockerFactorPredicates();
    testHasReverbZonesPredicate();
    testComputeListenerReverbBlendGuard();
    testWetMixEpsilonPredicates();
    testSpatialMixerOcclusionBlockerGuard();

    if (g_failures == 0) {
        std::printf("fuse_audio_occlusion_reverb_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_audio_occlusion_reverb_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
