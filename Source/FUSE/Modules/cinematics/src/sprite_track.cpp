#include <fuse/cinematics/sprite_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

namespace {

float sample_scalar_rail(const std::vector<SpriteKeyframe>& keyframes,
                         TimelineMs time_ms,
                         float (SpriteKeyframe::*member),
                         EaseMode ease) {
    const SpriteKeyframe* previous = nullptr;
    for (const SpriteKeyframe& keyframe : keyframes) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.*member;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.*member;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp(previous->*member, keyframe.*member, t);
        }

        previous = &keyframe;
    }

    return previous ? previous->*member : 0.f;
}

} // namespace

SpriteTrack::SpriteTrack(const std::string& label) : Track(label) {}

void SpriteTrack::add_keyframe(const SpriteKeyframe& keyframe) {
    keyframes_.push_back(keyframe);
}

void SpriteTrack::sort_keyframes() {
    std::stable_sort(keyframes_.begin(), keyframes_.end(),
                     [](const SpriteKeyframe& a, const SpriteKeyframe& b) {
                         return a.time_ms < b.time_ms;
                     });
}

SpriteSample SpriteTrack::sample_at(TimelineMs time_ms, EaseMode ease) const {
    if (keyframes_.empty()) {
        return {};
    }

    SpriteSample sample;
    sample.x = sample_scalar_rail(keyframes_, time_ms, &SpriteKeyframe::x, ease);
    sample.y = sample_scalar_rail(keyframes_, time_ms, &SpriteKeyframe::y, ease);
    sample.alpha = sample_scalar_rail(keyframes_, time_ms, &SpriteKeyframe::alpha, ease);
    return sample;
}

} // namespace fuse::cinematics
