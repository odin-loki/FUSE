#pragma once

// Ore: Engine/source/Verve/Core/VGroup.h
//      third_party/addons/Verve/Engine/source/Verve/Core/VGroup.h

#include <fuse/cinematics/audio_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/event_track.hpp>
#include <fuse/cinematics/property_track.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/cinematics/track.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::cinematics {

/// Groups related tracks (Verve VGroup).
class TrackGroup {
public:
    explicit TrackGroup(const std::string& label = "DefaultGroup");

    const std::string& label() const { return label_; }
    void set_label(const std::string& label) { label_ = label; }

    const std::vector<std::unique_ptr<Track>>& tracks() const { return tracks_; }

    Track& add_track(const std::string& label = "DefaultTrack");
    CameraTrack& add_camera_track(const std::string& label = "CameraTrack");
    SpriteTrack& add_sprite_track(const std::string& label = "SpriteTrack");
    PropertyTrack& add_property_track(const std::string& label = "PropertyTrack");
    AudioTrack& add_audio_track(const std::string& label = "AudioTrack");
    EventTrack& add_event_track(const std::string& label = "EventTrack");

    TrackSpan span() const;

private:
    std::string label_;
    std::vector<std::unique_ptr<Track>> tracks_;
};

} // namespace fuse::cinematics
