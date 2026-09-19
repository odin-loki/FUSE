#include <fuse/cinematics/actor_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

ActorTrack::ActorTrack(const std::string& label) : Track(label) {}

void ActorTrack::add_actor_event(const ActorEvent& event) {
    actor_events_.push_back(event);
}

void ActorTrack::sort_actor_events() {
    std::sort(actor_events_.begin(), actor_events_.end(), [](const ActorEvent& a, const ActorEvent& b) {
        return a.time_ms < b.time_ms;
    });
}

std::string ActorTrack::mount_point_at(TimelineMs time_ms) const {
    std::string mount_point;
    for (const ActorEvent& event : actor_events_) {
        if (event.time_ms > time_ms) {
            break;
        }
        if (event.kind == ActorEventKind::Mount) {
            mount_point = event.mount_point;
        } else {
            mount_point.clear();
        }
    }
    return mount_point;
}

float ActorTrack::mount_yaw_at(TimelineMs time_ms) const {
    float yaw_deg = 0.f;
    for (const ActorEvent& event : actor_events_) {
        if (event.time_ms > time_ms) {
            break;
        }
        if (event.kind == ActorEventKind::Mount) {
            yaw_deg = event.mount_yaw_deg;
        } else {
            yaw_deg = 0.f;
        }
    }
    return yaw_deg;
}

} // namespace fuse::cinematics
