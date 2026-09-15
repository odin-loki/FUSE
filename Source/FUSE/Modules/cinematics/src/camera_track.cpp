#include <fuse/cinematics/camera_track.hpp>

#include <fuse/cinematics/look_at.hpp>

#include <algorithm>
#include <cmath>

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

constexpr float kLookDirectionEpsilon = 1e-6f;
constexpr Vec3 kDefaultLookForward{0.f, 0.f, -1.f};

} // namespace

CameraKeyframeBracket find_camera_keyframe_bracket(const std::vector<CameraKeyframe>& keyframes,
                                                 TimelineMs time_ms,
                                                 EaseMode ease) {
    CameraKeyframeBracket bracket;
    if (keyframes.empty()) {
        return bracket;
    }

    int index = 0;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            bracket.next_index = index;
            if (index > 0) {
                bracket.prev_index = index - 1;
                const TimelineMs span = keyframe.time_ms - keyframes[bracket.prev_index].time_ms;
                if (span > 0) {
                    const float raw_t = static_cast<float>(time_ms - keyframes[bracket.prev_index].time_ms)
                                        / static_cast<float>(span);
                    bracket.segment_t = apply_ease(ease, raw_t);
                }
            }
            return bracket;
        }

        ++index;
    }

    bracket.prev_index = static_cast<int>(keyframes.size()) - 1;
    bracket.segment_t = 1.f;
    return bracket;
}

Vec3 sample_camera_position(const std::vector<CameraKeyframe>& keyframes,
                            TimelineMs time_ms,
                            EaseMode ease) {
    return sample_vec3_rail(keyframes, time_ms, &CameraKeyframe::position, ease);
}

float sample_camera_field_of_view(const std::vector<CameraKeyframe>& keyframes,
                                  TimelineMs time_ms,
                                  EaseMode ease) {
    return sample_fov_rail(keyframes, time_ms, ease);
}

float sample_camera_roll(const std::vector<CameraKeyframe>& keyframes,
                         TimelineMs time_ms,
                         EaseMode ease) {
    return sample_scalar_rail(keyframes, time_ms, &CameraKeyframe::roll_deg, ease);
}

Vec3 sample_camera_look_at(const std::vector<CameraKeyframe>& keyframes,
                           TimelineMs time_ms,
                           EaseMode ease,
                           const LookAtResolver* look_at_resolver) {
    return sample_look_at_rail(keyframes, time_ms, ease, look_at_resolver);
}

Vec3 camera_look_direction(const Vec3& position, const Vec3& look_at) {
    const Vec3 delta{look_at.x - position.x, look_at.y - position.y, look_at.z - position.z};
    const float length_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (length_sq <= kLookDirectionEpsilon * kLookDirectionEpsilon) {
        return kDefaultLookForward;
    }

    const float inv_length = 1.f / std::sqrt(length_sq);
    return {delta.x * inv_length, delta.y * inv_length, delta.z * inv_length};
}

float camera_look_distance(const Vec3& position, const Vec3& look_at) {
    const float dx = look_at.x - position.x;
    const float dy = look_at.y - position.y;
    const float dz = look_at.z - position.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 CameraSample::look_direction() const {
    return camera_look_direction(position, look_at);
}

float CameraSample::look_distance() const {
    return camera_look_distance(position, look_at);
}

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
    sample.position = sample_camera_position(keyframes_, time_ms, ease);
    sample.look_at = sample_camera_look_at(keyframes_, time_ms, ease, look_at_resolver);
    sample.field_of_view = sample_camera_field_of_view(keyframes_, time_ms, ease);
    sample.roll_deg = sample_camera_roll(keyframes_, time_ms, ease);
    return sample;
}

} // namespace fuse::cinematics
