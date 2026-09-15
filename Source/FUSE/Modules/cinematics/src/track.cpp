#include <fuse/cinematics/track.hpp>

#include <algorithm>
#include <cmath>

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

float Track::interpolation_at(TimelineMs time_ms, TimelineMs sequence_duration_ms) const {
    if (sequence_duration_ms <= 0 || time_ms == sequence_duration_ms) {
        return 1.f;
    }

    if (events_.empty()) {
        return static_cast<float>(time_ms) / static_cast<float>(sequence_duration_ms);
    }

    TimelineMs last_time = 0;
    for (const TimelineEvent& event : events_) {
        if (!event.enabled()) {
            continue;
        }

        const TimelineMs start_time = event.start_ms();
        const TimelineMs finish_time = event.finish_ms();

        if (time_ms < start_time) {
            const TimelineMs segment = start_time - last_time;
            if (segment <= 0) {
                return 0.f;
            }
            return static_cast<float>(time_ms - last_time) / static_cast<float>(segment);
        }

        last_time = start_time;

        if (time_ms < finish_time) {
            const TimelineMs segment = finish_time - last_time;
            if (segment <= 0) {
                return 1.f;
            }
            return static_cast<float>(time_ms - last_time) / static_cast<float>(segment);
        }

        last_time = finish_time;
    }

    const TimelineMs tail = sequence_duration_ms - last_time;
    if (tail <= 0) {
        return 1.f;
    }
    return static_cast<float>(time_ms - last_time) / static_cast<float>(tail);
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
