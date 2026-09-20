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

/// Runtime query of the same compile-time gate (ABI/debug dumps).
[[nodiscard]] bool trackBUnlockedRuntime();
[[nodiscard]] bool trackBFeatureEnabledRuntime(TrackBFeature feature);

} // namespace fuse::core
