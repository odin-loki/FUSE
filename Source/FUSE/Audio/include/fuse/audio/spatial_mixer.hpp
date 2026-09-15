#pragma once

#include <fuse/audio/audio_bus.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/audio_desc.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

/// CPU spatial mixer — HRTF-lite panning and distance attenuation into an interleaved stereo buffer.
class SpatialMixer {
public:
    void configure(u32 sample_rate, u32 max_sources, bool hrtf_enabled);

    void set_occlusion_blockers(const AABB* blockers, u32 blocker_count);
    void clear_occlusion_blockers();

    void mix(const AudioRegistry& registry, const HandleMap<AudioClip>& clips, float dt,
             std::vector<float>& stereo_out, u32 frames);

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

    float sample_clip(const AudioClip& clip, float play_head, u32 channel) const;
    void apply_hrtf_pan(float mono_sample, const Vec3& rel, float attenuation, float& left,
                        float& right) const;
};

} // namespace fuse::audio
