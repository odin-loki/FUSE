#pragma once

// Ore: Engine/source/Verve/Extension/Camera/VCameraTrack.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/Camera/VCameraTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

struct CameraKeyframe {
    TimelineMs time_ms = 0;
    Vec3 position{};
    Vec3 look_at{};
    float field_of_view = 60.f;
};

struct CameraSample {
    Vec3 position{};
    Vec3 look_at{};
    float field_of_view = 60.f;
};

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
    void sort_keyframes();

    CameraSample sample_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::string target_camera_id_;
    std::vector<CameraKeyframe> keyframes_;
};

} // namespace fuse::cinematics
