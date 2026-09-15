#include <fuse/cinematics/timeline.hpp>

namespace fuse::cinematics {

void Timeline::addTrack(Track track) {
    m_tracks.push_back(std::move(track));
}

void Timeline::tick(const frame::FrameCtx& ctx) {
    if (!m_playing) {
        return;
    }
    m_playhead += ctx.dt;
}

float Timeline::duration() const {
    float maxEnd = 0.f;
    for (const Track& track : m_tracks) {
        if (track.endTime > maxEnd) {
            maxEnd = track.endTime;
        }
    }
    return maxEnd;
}

void Timeline::play() {
    m_playing = true;
}

void Timeline::stop() {
    m_playing = false;
}

} // namespace fuse::cinematics
