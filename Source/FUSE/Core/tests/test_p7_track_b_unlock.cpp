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

} // namespace

int main() {
    testDefaultBuildLocksTrackB();
    testVulkanProductionLockedByDefault();
    testAllFeaturesFollowUnlockGate();

    if (g_failures == 0) {
        std::printf("fuse_core_p7_track_b_unlock: Track B production gate is locked\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_p7_track_b_unlock: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
