#include <fuse/cinematics/group.hpp>

#include <algorithm>

namespace fuse::cinematics {

TrackGroup::TrackGroup(const std::string& label) : label_(label) {}

Track& TrackGroup::add_track(const std::string& label) {
    tracks_.push_back(std::make_unique<Track>(label));
    return *tracks_.back();
}

CameraTrack& TrackGroup::add_camera_track(const std::string& label) {
    tracks_.push_back(std::make_unique<CameraTrack>(label));
    return static_cast<CameraTrack&>(*tracks_.back());
}

SpriteTrack& TrackGroup::add_sprite_track(const std::string& label) {
    tracks_.push_back(std::make_unique<SpriteTrack>(label));
    return static_cast<SpriteTrack&>(*tracks_.back());
}

PropertyTrack& TrackGroup::add_property_track(const std::string& label) {
    tracks_.push_back(std::make_unique<PropertyTrack>(label));
    return static_cast<PropertyTrack&>(*tracks_.back());
}

AudioTrack& TrackGroup::add_audio_track(const std::string& label) {
    tracks_.push_back(std::make_unique<AudioTrack>(label));
    return static_cast<AudioTrack&>(*tracks_.back());
}

EventTrack& TrackGroup::add_event_track(const std::string& label) {
    tracks_.push_back(std::make_unique<EventTrack>(label));
    return static_cast<EventTrack&>(*tracks_.back());
}

TrackSpan TrackGroup::span() const {
    TrackSpan span;
    bool found = false;
    for (const std::unique_ptr<Track>& track : tracks_) {
        if (!track || !track->enabled()) {
            continue;
        }

        const TrackSpan track_span = track->span();
        if (track_span.empty()) {
            continue;
        }

        if (!found) {
            span = track_span;
            found = true;
        } else {
            span.start_ms = std::min(span.start_ms, track_span.start_ms);
            span.end_ms = std::max(span.end_ms, track_span.end_ms);
        }
    }
    return span;
}

} // namespace fuse::cinematics
