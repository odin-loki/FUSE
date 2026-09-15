#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

/// Interleaved PCM clip (mono or stereo, f32 samples).
struct AudioClip {
    std::vector<float> samples;
    u32 sample_rate = 0;
    u32 channel_count = 0;
    float duration = 0.f;

    bool load_wav(const char* path);
    bool load_from_pcm(const float* interleaved, u32 frame_count, u32 channels, u32 rate);

    u32 frame_count() const {
        if (channel_count == 0) {
            return 0;
        }
        return static_cast<u32>(samples.size()) / channel_count;
    }

    bool empty() const { return samples.empty(); }
};

} // namespace fuse::audio
