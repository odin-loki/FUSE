#include <fuse/core/track_b.hpp>

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

} // namespace

int main() {
    testDefaultBuildLocksTrackB();
    testVulkanProductionLockedByDefault();
    testAllFeaturesFollowUnlockGate();
    testHostFeatureIsScoped();

    if (g_failures == 0) {
        std::printf("fuse_core_p7_track_b_unlock: Track B production gate is locked\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_p7_track_b_unlock: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
