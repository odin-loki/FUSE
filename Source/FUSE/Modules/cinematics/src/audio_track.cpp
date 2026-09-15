#include <fuse/cinematics/audio_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

AudioTrack::AudioTrack(const std::string& label) : Track(label) {}

void AudioTrack::add_keyframe(const AudioKeyframe& keyframe) {
    keyframes_.push_back(keyframe);
}

void AudioTrack::sort_keyframes() {
    std::stable_sort(keyframes_.begin(), keyframes_.end(),
                     [](const AudioKeyframe& a, const AudioKeyframe& b) {
                         return a.time_ms < b.time_ms;
                     });
}

float AudioTrack::volume_at(TimelineMs time_ms, EaseMode ease) const {
    const AudioKeyframe* previous = nullptr;
    for (const AudioKeyframe& keyframe : keyframes_) {
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.volume;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.volume;
            }

            const float t = apply_ease(ease,
                                       static_cast<float>(time_ms - previous->time_ms)
                                           / static_cast<float>(span));
            return lerp(previous->volume, keyframe.volume, t);
        }

        previous = &keyframe;
    }

    return previous ? previous->volume : 1.f;
}

} // namespace fuse::cinematics
