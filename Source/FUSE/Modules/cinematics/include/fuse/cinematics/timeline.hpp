#pragma once

// Ore: Engine/source/Verve/Core/VController.h, VController.cpp (processTick, play/pause/stop)
//      third_party/addons/Verve/Engine/source/Verve/Core/VController.h

#include <fuse/cinematics/cue_queue.hpp>
#include <fuse/cinematics/group.hpp>
#include <fuse/cinematics/playhead.hpp>

#include <functional>
#include <string>
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

    /// Seek playhead without starting playback; optionally enqueue forward-crossed cues.
    void scrub_to(TimelineMs time_ms, bool enqueue_cues = true);

    CueQueue& cue_queue() { return cue_queue_; }
    const CueQueue& cue_queue() const { return cue_queue_; }

    void on_update(TimelineUpdateCallback callback);
    void on_controller_event(TimelineEventCallback callback);

    /// Sort events on every track (VController::sort).
    void sort_tracks();

    /// Clear consume-once fired-cue ledger (also called on loop / reset).
    void clear_consumed_cues();

private:
    void post_event(ControllerEvent event);
    void handle_sequence_end();
    void collect_cues_forward(TimelineMs from_ms, TimelineMs to_ms);
    static std::string make_cue_key(const std::string& group_label,
                                    const std::string& track_label,
                                    const std::string& event_label,
                                    TimelineMs trigger_ms);
    static CuePayload make_cue_payload(const Track& track, const TimelineEvent& event);
    bool is_cue_consumed(const std::string& cue_key) const;
    void mark_cue_consumed(const std::string& cue_key);

    Playhead playhead_;
    std::vector<TrackGroup> groups_;
    CueQueue cue_queue_;
    std::vector<std::string> consumed_cue_keys_;
    bool loop_ = false;
    TimelineUpdateCallback update_callback_;
    TimelineEventCallback event_callback_;
};

} // namespace fuse::cinematics
