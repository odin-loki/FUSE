#pragma once

#include <fuse/cinematics/track.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::cinematics {

/// Timeline director stub — game-thread tick, parallel key eval deferred.
/// TODO(U5 extract): VController from third_party/addons/Verve/Engine/source/Verve/Core/VController.h
class Timeline {
public:
    void addTrack(Track track);
    u32 trackCount() const { return static_cast<u32>(m_tracks.size()); }

    /// Advance playhead on game thread; does not mutate scene objects yet.
    void tick(const frame::FrameCtx& ctx);

    float playhead() const { return m_playhead; }
    bool isPlaying() const { return m_playing; }
    void play();
    void stop();

private:
    std::vector<Track> m_tracks;
    float m_playhead = 0.f;
    bool m_playing = false;
};

} // namespace fuse::cinematics
