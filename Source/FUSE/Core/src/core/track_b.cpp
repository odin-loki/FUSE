#include <fuse/core/track_b.hpp>

namespace fuse::core {

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
