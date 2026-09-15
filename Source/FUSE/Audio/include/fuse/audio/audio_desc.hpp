#pragma once

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/handle.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

struct AudioClip;

struct AudioDesc {
    u32 sample_rate = 48000;
    u32 frames_per_buf = 512;
    u32 max_sources = 256;
    u32 max_reverb_zones = 32;
    bool hrtf_enabled = true;
    bool cuda_reverb = true;
};

struct AudioSourceDesc {
    Handle<AudioClip> clip = Handle<AudioClip>::invalid();
    float volume = 1.f;
    float pitch = 1.f;
    float min_distance = 1.f;
    float max_distance = 50.f;
    AttenuationCurve attenuation = AttenuationCurve::Linear;
    float rolloff = 1.f;
    AudioBus bus = AudioBus::Sfx;
    float occlusion = 1.f;
    bool looping = false;
    bool spatial = true;
    bool play_on_awake = false;
};

} // namespace fuse::audio
