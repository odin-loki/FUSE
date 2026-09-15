#include <fuse/cinematics/event.hpp>

namespace fuse::cinematics {

TimelineEvent::TimelineEvent(const std::string& label, TimelineMs trigger_ms, TimelineMs duration_ms)
    : label_(label), trigger_ms_(trigger_ms), duration_ms_(duration_ms) {}

bool TimelineEvent::should_trigger_forward(TimelineMs time, TimelineMs delta) const {
    if (!enabled_ || delta <= 0) {
        return false;
    }
    const TimelineMs new_time = time + delta;
    return time < trigger_ms_ && new_time >= trigger_ms_;
}

bool TimelineEvent::contains_time(TimelineMs time) const {
    if (!enabled_) {
        return false;
    }
    return time >= start_ms() && time < finish_ms();
}

} // namespace fuse::cinematics
