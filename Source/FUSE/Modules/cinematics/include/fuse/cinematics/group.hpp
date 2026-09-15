#pragma once

// Ore: Engine/source/Verve/Core/VGroup.h
//      third_party/addons/Verve/Engine/source/Verve/Core/VGroup.h

#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

/// Groups related tracks (Verve VGroup).
class TrackGroup {
public:
    explicit TrackGroup(const std::string& label = "DefaultGroup");

    const std::string& label() const { return label_; }
    void set_label(const std::string& label) { label_ = label; }

    const std::vector<Track>& tracks() const { return tracks_; }
    std::vector<Track>& tracks() { return tracks_; }

    Track& add_track(const std::string& label = "DefaultTrack");

private:
    std::string label_;
    std::vector<Track> tracks_;
};

} // namespace fuse::cinematics
