#pragma once

// Ore: Engine/source/Verve/Extension/SceneObject/VSceneObjectTrack.h (2D sprite binding stub)
//      third_party/addons/Verve/Engine/source/Verve/Extension/SceneObject/VSceneObjectTrack.h

#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/track.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

struct SpriteKeyframe {
    TimelineMs time_ms = 0;
    float x = 0.f;
    float y = 0.f;
    float alpha = 1.f;
};

struct SpriteSample {
    float x = 0.f;
    float y = 0.f;
    float alpha = 1.f;
};

/// 2D sprite transform lane stub (Verve scene-object track distilled for World2D).
class SpriteTrack : public Track {
public:
    explicit SpriteTrack(const std::string& label = "SpriteTrack");

    TrackKind kind() const override { return TrackKind::Sprite; }

    const std::string& target_sprite_id() const { return target_sprite_id_; }
    void set_target_sprite_id(const std::string& id) { target_sprite_id_ = id; }

    const std::vector<SpriteKeyframe>& keyframes() const { return keyframes_; }
    std::vector<SpriteKeyframe>& keyframes() { return keyframes_; }

    void add_keyframe(const SpriteKeyframe& keyframe);
    void sort_keyframes();

    SpriteSample sample_at(TimelineMs time_ms, EaseMode ease = EaseMode::Linear) const;

private:
    std::string target_sprite_id_;
    std::vector<SpriteKeyframe> keyframes_;
};

} // namespace fuse::cinematics
