#pragma once

// Ore: VController time/duration/status fields (VController.h)
//      third_party/addons/Verve/Engine/source/Verve/Core/VController.h

#include <fuse/cinematics/types.hpp>

namespace fuse::cinematics {

/// Current read position on a timeline (Verve VController playhead).
class Playhead {
public:
    Playhead() = default;

    TimelineMs time_ms() const { return time_ms_; }
    TimelineMs duration_ms() const { return duration_ms_; }
    TimeScale time_scale() const { return time_scale_; }
    PlaybackState state() const { return state_; }

    bool is_playing() const { return state_ == PlaybackState::Playing; }
    bool is_paused() const { return state_ == PlaybackState::Paused; }
    bool is_stopped() const { return state_ == PlaybackState::Stopped; }
    bool is_playing_forward() const { return time_scale_ > 0.f; }

    void set_time_ms(TimelineMs time_ms) { time_ms_ = time_ms; }
    void set_duration_ms(TimelineMs duration_ms) { duration_ms_ = duration_ms; }
    void set_time_scale(TimeScale scale) { time_scale_ = scale; }
    void set_state(PlaybackState state) { state_ = state; }

    /// Clamp time into [0, duration] when duration is positive.
    void clamp_time();

private:
    TimelineMs time_ms_ = 0;
    TimelineMs duration_ms_ = 5000;
    TimeScale time_scale_ = 1.f;
    PlaybackState state_ = PlaybackState::Stopped;
};

} // namespace fuse::cinematics
