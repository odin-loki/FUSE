#pragma once

// Ore: Engine/source/Verve/Extension/Camera/VCameraTrack.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/Camera/VCameraTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>
#include <cstddef>

namespace fuse::cinematics {

class LookAtResolver;
class CameraTrack;

/// How a camera keyframe resolves its look-at point.
enum class CameraLookAtMode {
    /// World-space point stored on the keyframe (`CameraKeyframe::look_at`).
    FixedPoint,
    /// Resolve world position from `look_at_target_id` via `LookAtResolver`.
    TargetEntity,
};

struct CameraKeyframe {
    TimelineMs time_ms = 0;
    Vec3 position{};
    CameraLookAtMode look_at_mode = CameraLookAtMode::FixedPoint;
    Vec3 look_at{};
    std::string look_at_target_id;
    float field_of_view = 60.f;
    float roll_deg = 0.f;
};

/// Default vertical FOV (degrees) for empty tracks and unset keyframes.
constexpr float kDefaultCameraFovDeg = 60.f;

/// Explicit default FOV accessor for editor / import wiring.
constexpr float default_camera_fov_deg() { return kDefaultCameraFovDeg; }

/// Default look-at distance (world units along -Z) when a fixed keyframe omits aim.
constexpr float kDefaultCameraLookAtDistance = 10.f;

struct CameraSample {
    Vec3 position{};
    Vec3 look_at{};
    float field_of_view = kDefaultCameraFovDeg;
    float roll_deg = 0.f;

    /// Unit vector from `position` toward `look_at` (default forward when coincident).
    Vec3 look_direction() const;

    /// Euclidean distance between `position` and `look_at`.
    float look_distance() const;
};

/// Normalized direction from camera position to look-at point.
Vec3 camera_look_direction(const Vec3& position, const Vec3& look_at);

/// Distance between camera position and look-at point.
float camera_look_distance(const Vec3& position, const Vec3& look_at);

/// Canonical pose when a track or keyframe list has no entries.
CameraSample default_camera_sample();

/// Default pose preview anchored at `position` (empty-track / editor placement stub).
CameraSample default_camera_sample_at(const Vec3& position, float look_distance = kDefaultCameraLookAtDistance);

/// True when `sample` matches `default_camera_sample()` (empty-track guard).
bool camera_sample_is_default(const CameraSample& sample);

/// Clamp FOV on a sampled pose; leaves other fields untouched (export / gameplay guard).
void normalize_camera_sample(CameraSample& sample);

/// True when `keyframes` has no entries (editor / rail guard).
bool camera_keyframes_empty(const std::vector<CameraKeyframe>& keyframes);

/// Readability alias for `CameraTrack::empty()`.
bool camera_track_is_empty(const CameraTrack& track);

/// Number of keyframes in `keyframes` (empty-vector guard for editor wiring).
size_t camera_keyframe_count(const std::vector<CameraKeyframe>& keyframes);

/// True when `time_ms` lies within the earliest..latest keyframe times (inclusive).
bool camera_track_covers_time(const std::vector<CameraKeyframe>& keyframes, TimelineMs time_ms);

/// True when `field_of_view` is unset (editor sentinel `<= 0` before normalization).
bool camera_keyframe_fov_unset(float field_of_view);

/// True when `fov_deg` is within `kMinFovDeg`..`kMaxFovDeg` (import / editor guard).
bool camera_fov_in_valid_range(float fov_deg);

/// Convenience inverse of `camera_fov_in_valid_range`.
bool camera_fov_needs_clamp(float fov_deg);

/// True when `field_of_view_deg` matches `kDefaultCameraFovDeg` (within epsilon).
bool camera_fov_uses_default(float field_of_view_deg);

/// Resolve authored FOV: unset values use `kDefaultCameraFovDeg`, otherwise clamp.
float effective_camera_fov(float field_of_view);

/// True when a fixed-point keyframe omits an explicit look-at point.
bool camera_keyframe_look_at_unset(const CameraKeyframe& keyframe);

/// Apply unset FOV / fixed look-at defaults, then clamp FOV (import / editor guard).
void apply_camera_keyframe_defaults(CameraKeyframe& keyframe);

/// Canonical keyframe with default FOV, roll, and look-at offset from `position`.
CameraKeyframe make_default_camera_keyframe(TimelineMs time_ms = 0);

/// True when `keyframe` binds look-at to an entity id (requires `LookAtResolver` stub).
bool camera_keyframe_uses_entity_look_at(const CameraKeyframe& keyframe);

/// True when entity look-at mode is active but `look_at_target_id` is empty (import guard).
bool camera_keyframe_entity_target_missing(const CameraKeyframe& keyframe);

/// True when any keyframe in `keyframes` requires a `LookAtResolver` at sample time.
bool camera_track_needs_look_at_resolver(const std::vector<CameraKeyframe>& keyframes);

/// Clamp FOV and leave other fields untouched (editor / import guard).
void normalize_camera_keyframe(CameraKeyframe& keyframe);

/// Clamp FOV on every keyframe in `keyframes` (batch import guard).
void normalize_camera_keyframes(std::vector<CameraKeyframe>& keyframes);

/// Reset `keyframe` to editor defaults (time 0, fixed look-at, default FOV).
void reset_camera_keyframe_to_defaults(CameraKeyframe& keyframe);

/// Sample a single keyframe without interpolation (hold pose stub).
CameraSample sample_camera_keyframe(const CameraKeyframe& keyframe,
                                    const LookAtResolver* look_at_resolver = nullptr);

/// Default world look-at point `distance` units along -Z from `position`.
Vec3 default_camera_look_at_for_position(const Vec3& position, float distance = 10.f);

/// Indices of bracketing keyframes for `time_ms` plus eased segment parameter.
struct CameraKeyframeBracket {
    int prev_index = -1;
    int next_index = -1;
    float segment_t = 0.f;
};

/// Find sorted keyframe indices surrounding `time_ms` (holds clamp before first / after last).
CameraKeyframeBracket find_camera_keyframe_bracket(const std::vector<CameraKeyframe>& keyframes,
                                                   TimelineMs time_ms,
                                                   EaseMode ease = EaseMode::Linear);

/// Sample individual camera rails without a `CameraTrack` wrapper.
Vec3 sample_camera_position(const std::vector<CameraKeyframe>& keyframes,
                            TimelineMs time_ms,
                            EaseMode ease = EaseMode::Linear);
float sample_camera_field_of_view(const std::vector<CameraKeyframe>& keyframes,
                                  TimelineMs time_ms,
                                  EaseMode ease = EaseMode::Linear);
float sample_camera_roll(const std::vector<CameraKeyframe>& keyframes,
                         TimelineMs time_ms,
                         EaseMode ease = EaseMode::Linear);
Vec3 sample_camera_look_at(const std::vector<CameraKeyframe>& keyframes,
                           TimelineMs time_ms,
                           EaseMode ease = EaseMode::Linear,
                           const LookAtResolver* look_at_resolver = nullptr);

/// Sample all camera rails into one pose without a `CameraTrack` wrapper.
CameraSample sample_camera_pose(const std::vector<CameraKeyframe>& keyframes,
                                TimelineMs time_ms,
                                EaseMode ease = EaseMode::Linear,
                                const LookAtResolver* look_at_resolver = nullptr);

/// Camera animation lane (Verve VCameraTrack / VSceneObjectTrack without Torque bridge).
class CameraTrack : public Track {
public:
    explicit CameraTrack(const std::string& label = "CameraTrack");

    TrackKind kind() const override { return TrackKind::Camera; }

    const std::string& target_camera_id() const { return target_camera_id_; }
    void set_target_camera_id(const std::string& id) { target_camera_id_ = id; }

    const std::vector<CameraKeyframe>& keyframes() const { return keyframes_; }
    std::vector<CameraKeyframe>& keyframes() { return keyframes_; }

    void add_keyframe(const CameraKeyframe& keyframe);
    void clear_keyframes();
    void sort_keyframes();
    void normalize_keyframes();

    bool empty() const { return keyframes_.empty(); }

    size_t keyframe_count() const { return keyframes_.size(); }

    /// True when `time_ms` lies within `keyframe_span()` (false when empty).
    bool covers_time(TimelineMs time_ms) const;

    /// True when any keyframe binds look-at to an entity id.
    bool needs_look_at_resolver() const;

    /// Earliest through latest keyframe time (requires sorted keyframes for tight bounds).
    TrackSpan keyframe_span() const;

    CameraSample sample_at(TimelineMs time_ms,
                           EaseMode ease = EaseMode::Linear,
                           const LookAtResolver* look_at_resolver = nullptr) const;

private:
    std::string target_camera_id_;
    std::vector<CameraKeyframe> keyframes_;
};

} // namespace fuse::cinematics
