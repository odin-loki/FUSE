#pragma once

#include <fuse/audio/audio_backend.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/audio_desc.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/audio/spatial_mixer.hpp>
#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

class AudioEngine {
public:
    void init(const AudioDesc& desc);
    void destroy();
    void update(AudioRegistry& registry, float dt);

    void play_at(Handle<AudioClip> clip, const Vec3& position, float volume = 1.f,
                 float pitch = 1.f);
    void play_2d(Handle<AudioClip> clip, float volume = 1.f);

    Handle<AudioClip> load_clip(const char* path);
    Handle<AudioClip> register_clip(AudioClip&& clip);
    void unload_clip(Handle<AudioClip> handle);

    struct ReverbZone {
        AABB bounds;
        Handle<AudioClip> impulse_response = Handle<AudioClip>::invalid();
        float wet_dry = 0.3f;
        float send_level = 1.f;
    };

    void add_reverb_zone(const ReverbZone& zone);
    void clear_reverb_zones();
    void set_reverb_send_level(float send_level);
    float reverb_send_level() const;

    bool is_initialized() const { return m_initialized; }
    const AudioDesc& desc() const { return m_desc; }
    AudioBackendKind backend_kind() const { return m_backend.kind(); }

    const std::vector<float>& last_mix_buffer() const { return m_mixBuffer; }
    const ConvReverbCpu& cpu_reverb() const { return m_cpuReverb; }

    AudioBusMixer& bus_mixer() { return m_mixer.bus_mixer(); }
    const AudioBusMixer& bus_mixer() const { return m_mixer.bus_mixer(); }

private:
    void apply_reverb_(std::vector<float>& stereo_buffer, u32 frames);
    void sync_backend_sources_(AudioRegistry& registry);

    AudioDesc m_desc{};
    bool m_initialized = false;
    AudioBackend m_backend;
    HandleMap<AudioClip> m_clips;
    std::vector<ReverbZone> m_reverbZones;
    SpatialMixer m_mixer;
    ConvReverbCpu m_cpuReverb;
    std::vector<float> m_mixBuffer;
    std::vector<float> m_dryBuffer;
};

} // namespace fuse::audio
