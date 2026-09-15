#include <fuse/cinematics/playhead.hpp>

#include <algorithm>

namespace fuse::cinematics {

void Playhead::clamp_time() {
    if (duration_ms_ <= 0) {
        time_ms_ = 0;
        return;
    }
    time_ms_ = std::clamp(time_ms_, TimelineMs{0}, duration_ms_);
}

void Playhead::scrub_to(TimelineMs time_ms) {
    time_ms_ = time_ms;
    clamp_time();
}

} // namespace fuse::cinematics
