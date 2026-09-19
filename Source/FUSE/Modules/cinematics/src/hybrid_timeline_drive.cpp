#include <fuse/cinematics/hybrid_timeline_drive.hpp>

#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/sprite_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

namespace {

const CameraTrack* find_first_camera_track(const Timeline& timeline) {
    for (const TrackGroup& group : timeline.groups()) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track != nullptr && track->enabled() && track->kind() == TrackKind::Camera) {
                return static_cast<const CameraTrack*>(track.get());
            }
        }
    }
    return nullptr;
}

const SpriteTrack* find_first_sprite_track(const Timeline& timeline) {
    for (const TrackGroup& group : timeline.groups()) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track != nullptr && track->enabled() && track->kind() == TrackKind::Sprite) {
                return static_cast<const SpriteTrack*>(track.get());
            }
        }
    }
    return nullptr;
}

} // namespace

HybridTimelineSample sample_hybrid_timeline_drive(const Timeline& timeline) {
    HybridTimelineSample sample;
    const TimelineMs time_ms = timeline.playhead().time_ms();

    if (const SpriteTrack* spriteTrack = find_first_sprite_track(timeline)) {
        const SpriteSample sprite = spriteTrack->sample_at(time_ms);
        sample.spriteX = sprite.x;
        sample.spriteY = sprite.y;
    }

    if (const CameraTrack* cameraTrack = find_first_camera_track(timeline)) {
        const CameraSample camera = cameraTrack->sample_at(time_ms);
        sample.clearR = 0.1f + clamp01(camera.position.y / 100.f) * 0.25f;
        sample.clearG = 0.15f + clamp01(camera.position.z / 20.f) * 0.15f;
        sample.clearB = 0.25f + clamp01((camera.field_of_view - 60.f) / 60.f) * 0.2f;
    }

    return sample;
}

} // namespace fuse::cinematics
