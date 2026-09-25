#pragma once

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/audio_desc.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/binaural_pan.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/audio/occlusion.hpp>
#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::audio {

/// Fire-and-forget voice started by \ref AudioEngine::play_at / play_2d.
struct OneShotVoice {
    Handle<AudioClip> clip = Handle<AudioClip>::invalid();
    Vec3 position{};
    float volume = 1.f;
    float pitch = 1.f;
    bool spatial = true;
    AudioBus bus = AudioBus::Sfx;
    double play_head = 0.0; ///< Seconds of clip time already rendered.
    u32 backend_source = 0;
    u32 id = 0;
};

/// CPU spatial mixer — HRTF-lite panning and distance attenuation into an interleaved stereo buffer.
class SpatialMixer {
public:
    void configure(u32 sample_rate, u32 max_sources, bool hrtf_enabled);

    void set_occlusion_blockers(const AABB* blockers, u32 blocker_count);
    void clear_occlusion_blockers();

    /// Effective visibility [0, 1] from per-source occlusion and registered blockers.
    float compute_source_visibility(const Vec3& listener, const Vec3& source,
                                    float source_occlusion) const;

    /// LF/HF occlusion attenuation from per-source occlusion and registered blockers.
    OcclusionAttenuation compute_source_occlusion_attenuation(const Vec3& listener,
                                                              const Vec3& source,
                                                              float source_occlusion) const;

    /// Binaural pan gains with empty-HRTF guards and distance/occlusion coupling.
    BinauralPanGains compute_source_binaural_pan_gains(const Vec3& rel_listener,
                                                         float distance_attenuation,
                                                         float occlusion_gain) const;

    /// Render \p frames of interleaved stereo. Voices are prioritised by audibility when more
    /// than `max_sources` are active; per-voice gains ramp over \ref kGainRampFrames to avoid
    /// zipper noise, and occluded voices run through a high-shelf (\ref kOcclusionCrossoverHz).
    void mix(const AudioRegistry& registry, const HandleMap<AudioClip>& clips, float dt,
             std::vector<float>& stereo_out, u32 frames);

    /// Advance registry and one-shot play heads by \p frames of output (the sample clock), wrap
    /// looping voices and retire finished one-shots. Returns backend ids of retired one-shots.
    std::vector<u32> advance(AudioRegistry& registry, const HandleMap<AudioClip>& clips, u32 frames);

    /// Queue a one-shot voice; returns its id.
    u32 add_one_shot(const OneShotVoice& voice);
    const std::vector<OneShotVoice>& one_shots() const { return m_oneShots; }
    void clear_one_shots() { m_oneShots.clear(); }

    /// Voices rendered by the last \ref mix call (after voice limiting).
    u32 last_mixed_voice_count() const { return m_lastMixedVoices; }
    /// Voices culled by the voice limit in the last \ref mix call.
    u32 last_culled_voice_count() const { return m_lastCulledVoices; }

    static constexpr u32 kGainRampFrames = 64;
    static constexpr float kOcclusionCrossoverHz = 2000.f;

    float last_master_gain() const { return m_lastMasterGain; }

    AudioBusMixer& bus_mixer() { return m_busMixer; }
    const AudioBusMixer& bus_mixer() const { return m_busMixer; }

private:
    u32 m_sampleRate = 48000;
    u32 m_maxSources = 256;
    bool m_hrtfEnabled = true;
    float m_lastMasterGain = 1.f;
    AudioBusMixer m_busMixer;
    std::vector<AABB> m_occlusionBlockers;
    std::vector<OneShotVoice> m_oneShots;
    u32 m_nextOneShotId = 1;
    u32 m_lastMixedVoices = 0;
    u32 m_lastCulledVoices = 0;

    /// Per-voice DSP state carried across buffers (gain ramp + occlusion shelf memory).
    struct VoiceState {
        float gain_left = 0.f;
        float gain_right = 0.f;
        float lp_left = 0.f;
        float lp_right = 0.f;
        bool primed = false;
        bool touched = false;
    };
    std::unordered_map<u64, VoiceState> m_voiceState;

    struct VoiceRequest {
        u64 key = 0;
        const AudioClip* clip = nullptr;
        Vec3 position{};
        float volume = 1.f;
        float pitch = 1.f;
        bool spatial = true;
        bool looping = false;
        double play_head = 0.0;
        AttenuationParams attenuation{};
        float occlusion = 1.f;
        AudioBus bus = AudioBus::Sfx;
        float audibility = 0.f;
    };
    /// Per-mix voice list, kept across calls so steady-state mixing does not touch the heap.
    std::vector<VoiceRequest> m_requests;

    float sample_clip(const AudioClip& clip, double frame_pos, u32 channel, bool looping) const;
    void render_voice(const VoiceRequest& voice, const Vec3& listener_pos, bool has_listener,
                      const ListenerBasis& basis, std::vector<float>& stereo_out, u32 frames);
};

} // namespace fuse::audio
