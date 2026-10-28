#include <fuse/core/track_b.hpp>
#include <fuse/core/track_token.hpp>
#include <fuse/gameplay/status_effect.hpp>

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

void testTrackTokenOk() {
    expectTrue(fuse::core::track_token_ok("B1"), "B1 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B2"), "B2 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B3"), "B3 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B4"), "B4 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B5"), "B5 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B6"), "B6 is a valid track token");
    expectTrue(fuse::core::track_token_ok("B7"), "B7 is a valid track token");
    expectTrue(!fuse::core::track_token_ok("B8"), "B8 is not a valid track token");
    expectTrue(!fuse::core::track_token_ok("b4"), "lowercase b4 is not a valid track token");
}

void testDefaultBuildLocksTrackB() {
    expectTrue(!fuse::core::trackBUnlocked(), "default build keeps Track B production locked");
    expectTrue(!fuse::core::trackBUnlockedRuntime(), "runtime query matches compile-time lock");
}

void testVulkanProductionLockedByDefault() {
    expectTrue(!fuse::core::trackBFeatureEnabled(fuse::core::TrackBFeature::VulkanProduction),
               "trackBFeatureEnabled(VulkanProduction)==false by default");
}

void testAllFeaturesFollowUnlockGate() {
    using fuse::core::TrackBFeature;
    expectTrue(!fuse::core::trackBFeatureEnabled(TrackBFeature::VulkanProduction),
               "VulkanProduction gated");
    expectTrue(!fuse::core::trackBFeatureEnabled(TrackBFeature::CudaProduction),
               "CudaProduction gated");
    expectTrue(!fuse::core::trackBFeatureEnabled(TrackBFeature::EditorGizmos),
               "EditorGizmos gated");
    expectTrue(!fuse::core::trackBFeatureEnabled(TrackBFeature::PbrFrame),
               "PbrFrame gated");
    expectTrue(!fuse::core::trackBFeatureEnabledRuntime(TrackBFeature::VulkanProduction),
               "runtime VulkanProduction gated");
    expectTrue(!fuse::core::trackBFeatureEnabledRuntime(TrackBFeature::CudaProduction),
               "runtime CudaProduction gated");
    expectTrue(!fuse::core::trackBFeatureEnabledRuntime(TrackBFeature::EditorGizmos),
               "runtime EditorGizmos gated");
    expectTrue(!fuse::core::trackBFeatureEnabledRuntime(TrackBFeature::PbrFrame),
               "runtime PbrFrame gated");
}

void testHostFeatureIsScoped() {
    using fuse::core::TrackBHostFeature;
    expectTrue(!fuse::core::trackBHostFeatureEnabled(TrackBHostFeature::EditorViewportPresent),
               "host-scoped editor present unlock off until a host opts in");
    fuse::core::setTrackBHostFeature(TrackBHostFeature::EditorViewportPresent, true);
#if !(defined(FUSE_SHIPPING) && FUSE_SHIPPING)
    expectTrue(fuse::core::trackBHostFeatureEnabled(TrackBHostFeature::EditorViewportPresent),
               "host opt-in enables only the host feature");
#endif
    expectTrue(!fuse::core::trackBUnlocked() && !fuse::core::trackBUnlockedRuntime(),
               "host opt-in never flips the compile-time Track B gate");
    expectTrue(!fuse::core::trackBFeatureEnabledRuntime(fuse::core::TrackBFeature::VulkanProduction),
               "host opt-in leaves VulkanProduction locked");
    fuse::core::setTrackBHostFeature(TrackBHostFeature::EditorViewportPresent, false);
    expectTrue(!fuse::core::trackBHostFeatureEnabled(TrackBHostFeature::EditorViewportPresent),
               "host opt-in can be withdrawn");
}

void testDamageOverTimeTick() {
    fuse::gameplay::HealthPool pool;
    pool.health = 10.f;
    pool.maxHealth = 10.f;
    pool.armor = 0.f;

    fuse::gameplay::DamageOverTime effect;
    effect.damagePerSec = 5.f;
    effect.remaining = 4.f;

    const fuse::f32 dealt = fuse::gameplay::tick_dot(effect, pool, 1.f);
    expectTrue(dealt == 5.f, "5 dps for 1s deals 5 damage");
    expectTrue(pool.health == 5.f, "5 dps for 1s leaves a 10-health pool at 5");
    expectTrue(effect.remaining == 3.f, "remaining decreases by 1");
}

} // namespace

int main() {
    testTrackTokenOk();
    testDefaultBuildLocksTrackB();
    testVulkanProductionLockedByDefault();
    testAllFeaturesFollowUnlockGate();
    testHostFeatureIsScoped();
    testDamageOverTimeTick();

    if (g_failures == 0) {
        std::printf("fuse_core_p7_track_b_unlock: Track B production gate is locked\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_p7_track_b_unlock: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
