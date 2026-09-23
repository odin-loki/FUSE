#pragma once

#include <fuse/types.hpp>

namespace fuse::core {

// Compile-time Track B production unlock (Track A P7).
// Scaffolding may still compile when this is false; production defaults must check it.
constexpr bool trackBUnlocked() {
#if defined(FUSE_TRACK_B_UNLOCK) && FUSE_TRACK_B_UNLOCK
    return true;
#else
    return false;
#endif
}

enum class TrackBFeature : u8 { VulkanProduction, CudaProduction, EditorGizmos, PbrFrame };

[[nodiscard]] constexpr bool trackBFeatureEnabled(TrackBFeature feature) {
    (void)feature;
    return trackBUnlocked();
}

/// Host-scoped Track B unlocks (P7: "Track B features merge only behind feature flags on FUSE APIs").
/// A few Track B paths belong to one host rather than to the engine: the Qt editor presents its
/// viewport into a surface the editor itself created on the FUSE VkInstance. That host opts in at
/// runtime, for its own process only, once the surface is real; `trackBUnlocked()` and every
/// `TrackBFeature` keep their compile-time answer. Ignored in FUSE_SHIPPING builds.
enum class TrackBHostFeature : u8 {
    /// `vkQueuePresentKHR` for the editor viewport swapchain on a Qt-owned VkSurfaceKHR.
    EditorViewportPresent,
};

void setTrackBHostFeature(TrackBHostFeature feature, bool enabled);
[[nodiscard]] bool trackBHostFeatureEnabled(TrackBHostFeature feature);

/// Runtime query of the same compile-time gate (ABI/debug dumps).
[[nodiscard]] bool trackBUnlockedRuntime();
[[nodiscard]] bool trackBFeatureEnabledRuntime(TrackBFeature feature);

} // namespace fuse::core
