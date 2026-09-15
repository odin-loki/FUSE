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

void Timeline::play() {
    m_playing = true;
}

void Timeline::stop() {
    m_playing = false;
}

} // namespace fuse::cinematics
