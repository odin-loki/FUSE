#pragma once

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/handle.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

struct AudioClip;

/// Which output device the engine opens. Auto tries OpenAL and falls back to Null.
enum class AudioOutputRequest : u8 { Auto, Null, OpenAL };

/// Who sets the sample clock.
///   Caller: every AudioEngine::update renders exactly one block of `frames_per_buf` (headless
///           tests, offline rendering, fixed-step hosts). With a real device, blocks that do not
///           fit the output queue are dropped and counted.
///   Device: every update renders as many blocks as the output queue can take right now (0..N),
///           so the device never starves as long as update runs more often than the queued
///           latency. This is what a runtime host with a real device wants. Null always behaves
///           as Caller (it has no clock of its own).
enum class AudioPacing : u8 { Caller, Device };

struct AudioDesc {
    u32 sample_rate = 48000;
    u32 frames_per_buf = 512;
    u32 max_sources = 256;
    u32 max_reverb_zones = 32;
    bool hrtf_enabled = true;
    bool cuda_reverb = true;

    // ---- device output (GAP-AUDIO-DEVICE-OUT) ----
    AudioOutputRequest output = AudioOutputRequest::Auto;
    AudioPacing pacing = AudioPacing::Caller;
    /// OpenAL: buffers (of `frames_per_buf` stereo frames) queued on the one streaming source (2..8).
    u32 device_buffers = 4;
    /// OpenAL: blocks the game-thread -> feeder-thread SPSC ring holds ahead of the device (>= 1).
    u32 output_ring_blocks = 4;
    /// Null: stereo frames kept in the capture buffer (reserved at init; 0 = no capture).
    u32 capture_frames = 0;

    // ---- clip loading (MP-B7.2-OGG-RUNTIME) ----
    /// Ogg Vorbis clips longer than this (seconds) stay encoded and play as streaming voices
    /// through a bounded per-voice decode ring; shorter ones are decoded to PCM at load. <= 0
    /// decodes everything.
    float stream_threshold_seconds = 10.f;
};

struct AudioSourceDesc {
    Handle<AudioClip> clip = Handle<AudioClip>::invalid();
    float volume = 1.f;
    float pitch = 1.f;
    float min_distance = 1.f;
    float max_distance = 50.f;
    AttenuationCurve attenuation = AttenuationCurve::Linear;
    float rolloff = 1.f;
    AttenuationKeypoint attenuation_keypoints[AttenuationParams::max_keypoints] = {};
    u32 attenuation_keypoint_count = 0;
    AudioBus bus = AudioBus::Sfx;
    float occlusion = 1.f;
    bool looping = false;
    bool spatial = true;
    bool play_on_awake = false;
};

} // namespace fuse::audio
