#pragma once

// Ore: Engine/source/Verve/Core/VTrack.h, VTrack.cpp (sort, span, next-event walk)
//      third_party/addons/Verve/Engine/source/Verve/Core/VTrack.h

#include <fuse/cinematics/event.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

enum class TrackKind {
    Generic,
    Camera,
    Sprite,
    Property,
    Audio,
    Event,
};

struct TrackSpan {
    TimelineMs start_ms = 0;
    TimelineMs end_ms = 0;

    TimelineMs length_ms() const { return end_ms - start_ms; }
    bool empty() const { return end_ms <= start_ms; }
};

/// Ordered event lane on the timeline (Verve VTrack).
class Track {
public:
    explicit Track(const std::string& label = "DefaultTrack");
    virtual ~Track() = default;

    virtual TrackKind kind() const { return TrackKind::Generic; }

    const std::string& label() const { return label_; }
    void set_label(const std::string& label) { label_ = label; }

    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    const std::vector<TimelineEvent>& events() const { return events_; }
    std::vector<TimelineEvent>& events() { return events_; }

    void add_event(const TimelineEvent& event);
    void sort_events();

    /// Earliest trigger through latest finish across enabled events.
    TrackSpan span() const;

    /// Normalized position within the track at `time_ms` (0..1), Verve calculateInterp.
    float interpolation_at(TimelineMs time_ms,
                           TimelineMs sequence_duration_ms,
                           bool playing_forward = true) const;

    /// Index of the next enabled event at or after `time_ms`, or -1.
    int next_event_index(TimelineMs time_ms) const;

protected:
    std::string label_;
    bool enabled_ = true;
    std::vector<TimelineEvent> events_;
};

} // namespace fuse::cinematics
