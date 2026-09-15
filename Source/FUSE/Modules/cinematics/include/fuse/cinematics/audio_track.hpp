#pragma once

// Ore: Engine/source/Verve/Extension/SoundEffect/VSoundEffectTrack.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/SoundEffect/VSoundEffectTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

struct AudioKeyframe {
    TimelineMs time_ms = 0;
    float volume = 1.f;
};

/// Sound-effect lane stub (Verve VSoundEffectTrack without Torque audio bridge).
class AudioTrack : public Track {
public:
    explicit AudioTrack(const std::string& label = "AudioTrack");

    TrackKind kind() const override { return TrackKind::Audio; }

    const std::string& sound_asset_id() const { return sound_asset_id_; }
    void set_sound_asset_id(const std::string& id) { sound_asset_id_ = id; }

    const std::vector<AudioKeyframe>& keyframes() const { return keyframes_; }
    std::vector<AudioKeyframe>& keyframes() { return keyframes_; }

    void add_keyframe(const AudioKeyframe& keyframe);
    void sort_keyframes();

    float volume_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::string sound_asset_id_;
    std::vector<AudioKeyframe> keyframes_;
};

} // namespace fuse::cinematics
