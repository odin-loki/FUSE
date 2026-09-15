#pragma once

// Ore: Engine/source/Verve/Core/VEvent.h
//      third_party/addons/Verve/Engine/source/Verve/Core/VEvent.h

#include <fuse/cinematics/types.hpp>

#include <string>

namespace fuse::cinematics {

/// A keyed moment on a track (Verve VEvent: trigger time + duration).
class TimelineEvent {
public:
    TimelineEvent() = default;
    TimelineEvent(const std::string& label, TimelineMs trigger_ms, TimelineMs duration_ms = 0);

    const std::string& label() const { return label_; }
    TimelineMs trigger_ms() const { return trigger_ms_; }
    TimelineMs duration_ms() const { return duration_ms_; }
    bool enabled() const { return enabled_; }

    void set_label(const std::string& label) { label_ = label; }
    void set_trigger_ms(TimelineMs trigger_ms) { trigger_ms_ = trigger_ms; }
    void set_duration_ms(TimelineMs duration_ms) { duration_ms_ = duration_ms; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    TimelineMs start_ms() const { return trigger_ms_; }
    TimelineMs finish_ms() const { return trigger_ms_ + duration_ms_; }

    /// True when [time, time+delta) crosses the trigger boundary forward.
    bool should_trigger_forward(TimelineMs time, TimelineMs delta) const;
    /// True when playhead sits inside the event's active span.
    bool contains_time(TimelineMs time) const;

private:
    std::string label_;
    TimelineMs trigger_ms_ = 0;
    TimelineMs duration_ms_ = 0;
    bool enabled_ = true;
};

} // namespace fuse::cinematics
