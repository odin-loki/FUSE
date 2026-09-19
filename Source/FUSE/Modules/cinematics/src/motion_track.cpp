#include <fuse/cinematics/motion_track.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::cinematics {

namespace {

float segment_length(const Vec3& a, const Vec3& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

void MotionPath::add_waypoint(const MotionWaypoint& waypoint) {
    waypoints_.push_back(waypoint);
}

void MotionPath::sort_waypoints() {
    std::stable_sort(waypoints_.begin(), waypoints_.end(),
                     [](const MotionWaypoint& a, const MotionWaypoint& b) {
                         return a.time_ms < b.time_ms;
                     });
}

float MotionPath::total_length() const {
    if (waypoints_.size() < 2) {
        return 0.f;
    }

    float length = 0.f;
    for (std::size_t i = 1; i < waypoints_.size(); ++i) {
        length += segment_length(waypoints_[i - 1].position, waypoints_[i].position);
    }
    return length;
}

MotionSample MotionPath::sample_at(TimelineMs time_ms, EaseMode ease) const {
    MotionSample sample;
    if (waypoints_.empty()) {
        return sample;
    }

    if (waypoints_.size() == 1) {
        sample.position = waypoints_.front().position;
        sample.path_param = 0.f;
        return sample;
    }

    const MotionWaypoint* previous = nullptr;
    for (const MotionWaypoint& waypoint : waypoints_) {
        if (time_ms < waypoint.time_ms) {
            if (!previous) {
                sample.position = waypoint.position;
                sample.path_param = 0.f;
                return sample;
            }

            const TimelineMs span = waypoint.time_ms - previous->time_ms;
            if (span <= 0) {
                sample.position = waypoint.position;
                sample.path_param = 1.f;
                return sample;
            }

            const float t = apply_ease(ease,
                                         static_cast<float>(time_ms - previous->time_ms)
                                             / static_cast<float>(span));
            sample.position = lerp_vec3(previous->position, waypoint.position, t);
            sample.path_param = clamp01(t);
            return sample;
        }

        previous = &waypoint;
    }

    sample.position = previous->position;
    sample.path_param = 1.f;
    return sample;
}

MotionTrack::MotionTrack(const std::string& label) : Track(label) {}

MotionSample MotionTrack::sample_at(TimelineMs time_ms, EaseMode ease) const {
    return path_.sample_at(time_ms, ease);
}

} // namespace fuse::cinematics
