#include <fuse/cinematics/camera_track.hpp>

#include <fuse/cinematics/look_at.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

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

    return previous ? clamp_fov(previous->field_of_view) : kDefaultCameraFovDeg;
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

CameraSample default_camera_sample() {
    return {};
}

void normalize_camera_sample(CameraSample& sample) {
    sample.field_of_view = clamp_fov(sample.field_of_view);
}

bool camera_keyframes_empty(const std::vector<CameraKeyframe>& keyframes) {
    return keyframes.empty();
}

bool camera_track_is_empty(const CameraTrack& track) {
    return track.empty();
}

size_t camera_keyframe_count(const std::vector<CameraKeyframe>& keyframes) {
    return keyframes.size();
}

bool camera_track_covers_time(const std::vector<CameraKeyframe>& keyframes, TimelineMs time_ms) {
    if (keyframes.empty()) {
        return false;
    }

    TimelineMs start_ms = keyframes.front().time_ms;
    TimelineMs end_ms = keyframes.front().time_ms;
    for (const CameraKeyframe& keyframe : keyframes) {
        start_ms = std::min(start_ms, keyframe.time_ms);
        end_ms = std::max(end_ms, keyframe.time_ms);
    }

    return time_ms >= start_ms && time_ms <= end_ms;
}

bool camera_keyframe_fov_unset(float field_of_view) {
    return field_of_view <= 0.f;
}

bool camera_fov_in_valid_range(float fov_deg) {
    return fov_deg >= kMinFovDeg && fov_deg <= kMaxFovDeg;
}

bool camera_fov_needs_clamp(float fov_deg) {
    return !camera_fov_in_valid_range(fov_deg);
}

float effective_camera_fov(float field_of_view) {
    if (camera_keyframe_fov_unset(field_of_view)) {
        return kDefaultCameraFovDeg;
    }
    return clamp_fov(field_of_view);
}

bool camera_keyframe_look_at_unset(const CameraKeyframe& keyframe) {
    if (keyframe.look_at_mode != CameraLookAtMode::FixedPoint) {
        return false;
    }

    return keyframe.look_at.x == 0.f && keyframe.look_at.y == 0.f && keyframe.look_at.z == 0.f;
}

void apply_camera_keyframe_defaults(CameraKeyframe& keyframe) {
    if (camera_keyframe_fov_unset(keyframe.field_of_view)) {
        keyframe.field_of_view = kDefaultCameraFovDeg;
    } else {
        keyframe.field_of_view = clamp_fov(keyframe.field_of_view);
    }

    if (camera_keyframe_look_at_unset(keyframe)) {
        keyframe.look_at = default_camera_look_at_for_position(keyframe.position, kDefaultCameraLookAtDistance);
    }
}

CameraKeyframe make_default_camera_keyframe(TimelineMs time_ms) {
    CameraKeyframe keyframe;
    keyframe.time_ms = time_ms;
    keyframe.field_of_view = kDefaultCameraFovDeg;
    keyframe.look_at = default_camera_look_at_for_position(keyframe.position, kDefaultCameraLookAtDistance);
    return keyframe;
}

bool camera_keyframe_uses_entity_look_at(const CameraKeyframe& keyframe) {
    return keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty();
}

bool camera_keyframe_entity_target_missing(const CameraKeyframe& keyframe) {
    return keyframe.look_at_mode == CameraLookAtMode::TargetEntity && keyframe.look_at_target_id.empty();
}

bool camera_track_needs_look_at_resolver(const std::vector<CameraKeyframe>& keyframes) {
    for (const CameraKeyframe& keyframe : keyframes) {
        if (camera_keyframe_uses_entity_look_at(keyframe)) {
            return true;
        }
    }
    return false;
}

void normalize_camera_keyframe(CameraKeyframe& keyframe) {
    keyframe.field_of_view = clamp_fov(keyframe.field_of_view);
}

void normalize_camera_keyframes(std::vector<CameraKeyframe>& keyframes) {
    for (CameraKeyframe& keyframe : keyframes) {
        normalize_camera_keyframe(keyframe);
    }
}

CameraSample sample_camera_keyframe(const CameraKeyframe& keyframe,
                                    const LookAtResolver* look_at_resolver) {
    LookAtResolver fallback;
    const LookAtResolver& resolver = look_at_resolver ? *look_at_resolver : fallback;

    CameraSample sample;
    sample.position = keyframe.position;
    sample.look_at = resolve_look_at_world(keyframe, resolver);
    sample.field_of_view = clamp_fov(keyframe.field_of_view);
    sample.roll_deg = keyframe.roll_deg;
    return sample;
}

Vec3 default_camera_look_at_for_position(const Vec3& position, float distance) {
    return {position.x, position.y, position.z - distance};
}

CameraSample default_camera_sample_at(const Vec3& position, float look_distance) {
    CameraSample sample;
    sample.position = position;
    sample.look_at = default_camera_look_at_for_position(position, look_distance);
    sample.field_of_view = kDefaultCameraFovDeg;
    sample.roll_deg = 0.f;
    return sample;
}

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
    if (keyframes.empty()) {
        return {};
    }
    return sample_vec3_rail(keyframes, time_ms, &CameraKeyframe::position, ease);
}

float sample_camera_field_of_view(const std::vector<CameraKeyframe>& keyframes,
                                  TimelineMs time_ms,
                                  EaseMode ease) {
    if (keyframes.empty()) {
        return kDefaultCameraFovDeg;
    }
    return sample_fov_rail(keyframes, time_ms, ease);
}

float sample_camera_roll(const std::vector<CameraKeyframe>& keyframes,
                         TimelineMs time_ms,
                         EaseMode ease) {
    if (keyframes.empty()) {
        return 0.f;
    }
    return sample_scalar_rail(keyframes, time_ms, &CameraKeyframe::roll_deg, ease);
}

Vec3 sample_camera_look_at(const std::vector<CameraKeyframe>& keyframes,
                           TimelineMs time_ms,
                           EaseMode ease,
                           const LookAtResolver* look_at_resolver) {
    if (keyframes.empty()) {
        return {};
    }
    return sample_look_at_rail(keyframes, time_ms, ease, look_at_resolver);
}

CameraSample sample_camera_pose(const std::vector<CameraKeyframe>& keyframes,
                                TimelineMs time_ms,
                                EaseMode ease,
                                const LookAtResolver* look_at_resolver) {
    if (keyframes.empty()) {
        return default_camera_sample();
    }

    CameraSample sample;
    sample.position = sample_camera_position(keyframes, time_ms, ease);
    sample.look_at = sample_camera_look_at(keyframes, time_ms, ease, look_at_resolver);
    sample.field_of_view = sample_camera_field_of_view(keyframes, time_ms, ease);
    sample.roll_deg = sample_camera_roll(keyframes, time_ms, ease);
    normalize_camera_sample(sample);
    return sample;
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
    CameraKeyframe normalized = keyframe;
    normalize_camera_keyframe(normalized);
    keyframes_.push_back(normalized);
}

bool CameraTrack::needs_look_at_resolver() const {
    return camera_track_needs_look_at_resolver(keyframes_);
}

bool CameraTrack::covers_time(TimelineMs time_ms) const {
    return camera_track_covers_time(keyframes_, time_ms);
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

void CameraTrack::normalize_keyframes() {
    normalize_camera_keyframes(keyframes_);
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
        return default_camera_sample();
    }

    CameraSample sample;
    sample.position = sample_camera_position(keyframes_, time_ms, ease);
    sample.look_at = sample_camera_look_at(keyframes_, time_ms, ease, look_at_resolver);
    sample.field_of_view = sample_camera_field_of_view(keyframes_, time_ms, ease);
    sample.roll_deg = sample_camera_roll(keyframes_, time_ms, ease);
    return sample;
}

} // namespace fuse::cinematics
