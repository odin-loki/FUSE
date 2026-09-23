#include <fuse/core/track_b.hpp>

#include <atomic>

namespace fuse::core {

namespace {
std::atomic<u32> g_hostFeatures{0};

constexpr u32 hostFeatureBit(TrackBHostFeature feature) {
    return 1u << static_cast<u32>(feature);
}
} // namespace

void setTrackBHostFeature(TrackBHostFeature feature, bool enabled) {
#if defined(FUSE_SHIPPING) && FUSE_SHIPPING
    (void)feature;
    (void)enabled;
#else
    if (enabled) {
        g_hostFeatures.fetch_or(hostFeatureBit(feature), std::memory_order_acq_rel);
    } else {
        g_hostFeatures.fetch_and(~hostFeatureBit(feature), std::memory_order_acq_rel);
    }
#endif
}

bool trackBHostFeatureEnabled(TrackBHostFeature feature) {
    return (g_hostFeatures.load(std::memory_order_acquire) & hostFeatureBit(feature)) != 0u;
}

bool trackBUnlockedRuntime() {
#if defined(FUSE_TRACK_B_UNLOCK) && FUSE_TRACK_B_UNLOCK
    return true;
#else
    return false;
#endif
}

bool trackBFeatureEnabledRuntime(TrackBFeature feature) {
    (void)feature;
    return trackBUnlockedRuntime();
}

} // namespace fuse::core
