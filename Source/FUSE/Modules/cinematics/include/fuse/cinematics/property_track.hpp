#pragma once

// Ore: Engine/source/Verve/Extension/Motion/VMotionTrack.h (scalar property rail stub)
//      third_party/addons/Verve/Engine/source/Verve/Extension/Motion/VMotionTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

struct PropertyKeyframe {
    TimelineMs time_ms = 0;
    float value = 0.f;
};

/// Generic float property lane stub (Verve motion/property events without Torque bridge).
class PropertyTrack : public Track {
public:
    explicit PropertyTrack(const std::string& label = "PropertyTrack");

    TrackKind kind() const override { return TrackKind::Property; }

    const std::string& property_path() const { return property_path_; }
    void set_property_path(const std::string& path) { property_path_ = path; }

    const std::string& target_object_id() const { return target_object_id_; }
    void set_target_object_id(const std::string& id) { target_object_id_ = id; }

    const std::vector<PropertyKeyframe>& keyframes() const { return keyframes_; }
    std::vector<PropertyKeyframe>& keyframes() { return keyframes_; }

    void add_keyframe(const PropertyKeyframe& keyframe);
    void sort_keyframes();

    float sample_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::string property_path_;
    std::string target_object_id_;
    std::vector<PropertyKeyframe> keyframes_;
};

} // namespace fuse::cinematics
