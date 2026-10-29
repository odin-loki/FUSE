#pragma once

// Ore: Engine/source/Verve/Extension/Motion/VMotionTrack.h (scalar property rail stub)
//      ore/torque-addon/Verve/Engine/source/Verve/Extension/Motion/VMotionTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

/// Segment interpolation leaving a key (applies from this key to the next).
enum class KeyInterp : unsigned char {
    Linear,   ///< straight line (default; legacy behaviour, honours the track EaseMode)
    Constant, ///< step: hold this key's value until the next key
    Cubic,    ///< cubic Hermite using out_tangent of this key and in_tangent of the next
};

/// How a key's tangents are derived (Cubic segments only).
enum class TangentMode : unsigned char {
    Auto, ///< Catmull-Rom style slope from the neighbours, clamped flat at local extrema (no overshoot)
    Flat, ///< zero slope (ease in / out)
    User, ///< in_tangent / out_tangent as stored (may be broken: in != out)
};

struct PropertyKeyframe {
    TimelineMs time_ms = 0;
    float value = 0.f;
    KeyInterp interp = KeyInterp::Linear;
    TangentMode tangent_mode = TangentMode::Auto;
    /// Slopes in value units per second (User mode; Auto / Flat recompute them).
    float in_tangent = 0.f;
    float out_tangent = 0.f;
};

const char* key_interp_name(KeyInterp interp);
const char* tangent_mode_name(TangentMode mode);
bool parse_key_interp(const std::string& name, KeyInterp& out);
bool parse_tangent_mode(const std::string& name, TangentMode& out);

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

    /// Recompute Auto / Flat tangents from the key values (call after edits; sort first).
    void update_tangents();
    /// Effective (in, out) slope of key `index` in value units per second (Auto / Flat resolved).
    void effective_tangents(size_t index, float& in_slope, float& out_slope) const;

private:
    std::string property_path_;
    std::string target_object_id_;
    std::vector<PropertyKeyframe> keyframes_;
};

} // namespace fuse::cinematics
