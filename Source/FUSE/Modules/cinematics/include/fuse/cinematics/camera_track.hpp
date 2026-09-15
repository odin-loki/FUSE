#pragma once

// Ore: Engine/source/Verve/Extension/Camera/VCameraTrack.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/Camera/VCameraTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

class LookAtResolver;

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

struct CameraSample {
    Vec3 position{};
    Vec3 look_at{};
    float field_of_view = 60.f;
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

    bool empty() const { return keyframes_.empty(); }

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
