#include <fuse/cinematics/property_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

PropertyTrack::PropertyTrack(const std::string& label) : Track(label) {}

void PropertyTrack::add_keyframe(const PropertyKeyframe& keyframe) {
    keyframes_.push_back(keyframe);
}

void PropertyTrack::sort_keyframes() {
    std::stable_sort(keyframes_.begin(), keyframes_.end(),
                     [](const PropertyKeyframe& a, const PropertyKeyframe& b) {
                         return a.time_ms < b.time_ms;
                     });
}

float PropertyTrack::sample_at(TimelineMs time_ms, EaseMode ease) const {
    const PropertyKeyframe* previous = nullptr;
    for (const PropertyKeyframe& keyframe : keyframes_) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.value;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.value;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp(previous->value, keyframe.value, t);
        }

        previous = &keyframe;
    }

    return previous ? previous->value : 0.f;
}

} // namespace fuse::cinematics
