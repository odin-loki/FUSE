#include <fuse/cinematics/camera_track.hpp>

#include <fuse/cinematics/look_at.hpp>

#include <algorithm>

namespace fuse::cinematics {

namespace {

float sample_scalar_rail(const std::vector<CameraKeyframe>& keyframes,
                         TimelineMs time_ms,
                         float (CameraKeyframe::*member),
                         EaseMode ease) {
    const CameraKeyframe* previous = nullptr;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.*member;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.*member;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp(previous->*member, keyframe.*member, t);
        }

        previous = &keyframe;
    }

    return previous ? previous->*member : 0.f;
}

Vec3 sample_vec3_rail(const std::vector<CameraKeyframe>& keyframes,
                      TimelineMs time_ms,
                      Vec3 (CameraKeyframe::*member),
                      EaseMode ease) {
    const CameraKeyframe* previous = nullptr;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.*member;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.*member;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp_vec3(previous->*member, keyframe.*member, t);
        }

        previous = &keyframe;
    }

    return previous ? previous->*member : Vec3{};
}

float sample_fov_rail(const std::vector<CameraKeyframe>& keyframes, TimelineMs time_ms, EaseMode ease) {
    const CameraKeyframe* previous = nullptr;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return clamp_fov(keyframe.field_of_view);
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return clamp_fov(keyframe.field_of_view);
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp_fov(previous->field_of_view, keyframe.field_of_view, t);
        }

        previous = &keyframe;
    }

    return previous ? clamp_fov(previous->field_of_view) : kMinFovDeg;
}

Vec3 sample_look_at_rail(const std::vector<CameraKeyframe>& keyframes,
                         TimelineMs time_ms,
                         EaseMode ease,
                         const LookAtResolver* look_at_resolver) {
    LookAtResolver fallback;
    const LookAtResolver& resolver = look_at_resolver ? *look_at_resolver : fallback;

    const CameraKeyframe* previous = nullptr;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            const Vec3 current = resolve_look_at_world(keyframe, resolver);
            if (!previous) {
                return current;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return current;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            const Vec3 from = resolve_look_at_world(*previous, resolver);
            return lerp_vec3(from, current, t);
        }

        previous = &keyframe;
    }

    return previous ? resolve_look_at_world(*previous, resolver) : Vec3{};
}

} // namespace

CameraTrack::CameraTrack(const std::string& label) : Track(label) {}

void CameraTrack::add_keyframe(const CameraKeyframe& keyframe) {
    keyframes_.push_back(keyframe);
}

void CameraTrack::clear_keyframes() {
    keyframes_.clear();
}

void CameraTrack::sort_keyframes() {
    std::stable_sort(keyframes_.begin(), keyframes_.end(),
                     [](const CameraKeyframe& a, const CameraKeyframe& b) {
                         return a.time_ms < b.time_ms;
                     });
}

TrackSpan CameraTrack::keyframe_span() const {
    if (keyframes_.empty()) {
        return {};
    }

    TimelineMs start_ms = keyframes_.front().time_ms;
    TimelineMs end_ms = keyframes_.front().time_ms;
    for (const CameraKeyframe& keyframe : keyframes_) {
        start_ms = std::min(start_ms, keyframe.time_ms);
        end_ms = std::max(end_ms, keyframe.time_ms);
    }

    return {start_ms, end_ms};
}

CameraSample CameraTrack::sample_at(TimelineMs time_ms,
                                    EaseMode ease,
                                    const LookAtResolver* look_at_resolver) const {
    if (keyframes_.empty()) {
        return {};
    }

    CameraSample sample;
    sample.position = sample_vec3_rail(keyframes_, time_ms, &CameraKeyframe::position, ease);
    sample.look_at = sample_look_at_rail(keyframes_, time_ms, ease, look_at_resolver);
    sample.field_of_view = sample_fov_rail(keyframes_, time_ms, ease);
    sample.roll_deg = sample_scalar_rail(keyframes_, time_ms, &CameraKeyframe::roll_deg, ease);
    return sample;
}

} // namespace fuse::cinematics
