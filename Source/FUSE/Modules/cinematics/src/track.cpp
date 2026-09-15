#include <fuse/cinematics/track.hpp>

#include <fuse/cinematics/interpolate.hpp>

#include <algorithm>

namespace fuse::cinematics {

Track::Track(const std::string& label) : label_(label) {}

void Track::add_event(const TimelineEvent& event) {
    events_.push_back(event);
}

void Track::sort_events() {
    std::stable_sort(events_.begin(), events_.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) {
                         return a.trigger_ms() < b.trigger_ms();
                     });
}

TrackSpan Track::span() const {
    TrackSpan span;
    bool found = false;
    for (const TimelineEvent& event : events_) {
        if (!event.enabled()) {
            continue;
        }
        if (!found) {
            span.start_ms = event.start_ms();
            span.end_ms = event.finish_ms();
            found = true;
        } else {
            span.start_ms = std::min(span.start_ms, event.start_ms());
            span.end_ms = std::max(span.end_ms, event.finish_ms());
        }
    }
    return span;
}

float Track::interpolation_at(TimelineMs time_ms,
                              TimelineMs sequence_duration_ms,
                              bool playing_forward) const {
    return calculate_track_interp_forward(playing_forward, events_, time_ms, sequence_duration_ms);
}

int Track::next_event_index(TimelineMs time_ms) const {
    for (std::size_t i = 0; i < events_.size(); ++i) {
        if (!events_[i].enabled()) {
            continue;
        }
        if (events_[i].trigger_ms() >= time_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace fuse::cinematics
