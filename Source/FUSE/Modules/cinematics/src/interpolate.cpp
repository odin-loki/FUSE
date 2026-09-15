#include <fuse/cinematics/interpolate.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::cinematics {

float clamp01(float t) {
    return std::max(0.f, std::min(1.f, t));
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

Vec3 lerp_vec3(const Vec3& a, const Vec3& b, float t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

float clamp_fov(float fov_deg) {
    return std::max(kMinFovDeg, std::min(kMaxFovDeg, fov_deg));
}

float lerp_fov(float a, float b, float t) {
    return clamp_fov(lerp(a, b, t));
}

float apply_ease(EaseMode mode, float t) {
    const float clamped = clamp01(t);
    switch (mode) {
    case EaseMode::Linear:
        return clamped;
    case EaseMode::SmoothStep:
        return clamped * clamped * (3.f - 2.f * clamped);
    }
    return clamped;
}

float calculate_track_interp(const std::vector<TimelineEvent>& events,
                             TimelineMs time_ms,
                             TimelineMs sequence_duration_ms) {
    if (sequence_duration_ms <= 0 || time_ms == sequence_duration_ms) {
        return 1.f;
    }

    if (events.empty()) {
        return static_cast<float>(time_ms) / static_cast<float>(sequence_duration_ms);
    }

    TimelineMs last_time = 0;
    for (const TimelineEvent& event : events) {
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

float calculate_track_interp_forward(bool playing_forward,
                                     const std::vector<TimelineEvent>& events,
                                     TimelineMs time_ms,
                                     TimelineMs sequence_duration_ms) {
    const float interp = calculate_track_interp(events, time_ms, sequence_duration_ms);
    return playing_forward ? interp : (1.f - interp);
}

} // namespace fuse::cinematics
