#pragma once

// Ore: Engine/source/Verve/Core/VController.h, VController.cpp (processTick, play/pause/stop)
//      third_party/addons/Verve/Engine/source/Verve/Core/VController.h

#include <fuse/cinematics/group.hpp>
#include <fuse/cinematics/playhead.hpp>

#include <functional>
#include <vector>

namespace fuse::cinematics {

using TimelineUpdateCallback = std::function<void(TimelineMs time_ms, TimelineMs delta_ms)>;
using TimelineEventCallback = std::function<void(ControllerEvent event)>;

/// Sequence director — distilled Verve VController without Torque/SimObject.
class Timeline {
public:
    Timeline();

    Playhead& playhead() { return playhead_; }
    const Playhead& playhead() const { return playhead_; }

    const std::vector<TrackGroup>& groups() const { return groups_; }
    std::vector<TrackGroup>& groups() { return groups_; }

    TrackGroup& add_group(const std::string& label = "DefaultGroup");

    bool loop() const { return loop_; }
    void set_loop(bool loop) { loop_ = loop; }

    /// Union span across all enabled tracks in all groups.
    TrackSpan content_span() const;

    /// Suggested sequence duration from track content (falls back to playhead duration).
    TimelineMs suggested_duration_ms() const;

    void reset(TimelineMs time_ms = 0);
    void play(TimelineMs time_ms = -1);
    void pause();
    void stop(bool reset_playhead = true);

    /// Advance playback by `delta_ms` (wall-clock ms × time scale applied internally).
    void advance(TimelineMs delta_ms);

    void on_update(TimelineUpdateCallback callback);
    void on_controller_event(TimelineEventCallback callback);

    /// Sort events on every track (VController::sort).
    void sort_tracks();

private:
    void post_event(ControllerEvent event);
    void handle_sequence_end();

    Playhead playhead_;
    std::vector<TrackGroup> groups_;
    bool loop_ = false;
    TimelineUpdateCallback update_callback_;
    TimelineEventCallback event_callback_;
};

} // namespace fuse::cinematics
