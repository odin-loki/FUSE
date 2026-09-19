#pragma once

// Ore: Engine/source/Verve/Extension/Motion/VMotionTrack.h, VPath/VPath.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/Motion/VMotionTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

enum class MotionOrientationMode {
    Fixed,
    PathTangent,
};

struct MotionWaypoint {
    TimelineMs time_ms = 0;
    Vec3 position{};
};

struct MotionSample {
    Vec3 position{};
    float path_param = 0.f;
};

/// Piecewise-linear path rail (Verve VPath linear segment ore without SceneObject bridge).
class MotionPath {
public:
    const std::vector<MotionWaypoint>& waypoints() const { return waypoints_; }
    std::vector<MotionWaypoint>& waypoints() { return waypoints_; }

    void add_waypoint(const MotionWaypoint& waypoint);
    void sort_waypoints();

    bool empty() const { return waypoints_.empty(); }
    float total_length() const;

    MotionSample sample_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::vector<MotionWaypoint> waypoints_;
};

/// Object motion lane along a path (Verve VMotionTrack without Torque PathObject bridge).
class MotionTrack : public Track {
public:
    explicit MotionTrack(const std::string& label = "MotionTrack");

    TrackKind kind() const override { return TrackKind::Motion; }

    const std::string& target_object_id() const { return target_object_id_; }
    void set_target_object_id(const std::string& id) { target_object_id_ = id; }

    const std::string& path_id() const { return path_id_; }
    void set_path_id(const std::string& id) { path_id_ = id; }

    MotionOrientationMode orientation_mode() const { return orientation_mode_; }
    void set_orientation_mode(MotionOrientationMode mode) { orientation_mode_ = mode; }

    bool relative() const { return relative_; }
    void set_relative(bool value) { relative_ = value; }

    const MotionPath& path() const { return path_; }
    MotionPath& path() { return path_; }

    MotionSample sample_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::string target_object_id_;
    std::string path_id_;
    MotionOrientationMode orientation_mode_ = MotionOrientationMode::PathTangent;
    bool relative_ = false;
    MotionPath path_;
};

} // namespace fuse::cinematics
