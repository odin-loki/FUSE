#pragma once

#include <fuse/audio/audio_backend.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/audio_desc.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/audio/reverb_zones.hpp>
#include <fuse/audio/spatial_mixer.hpp>
#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
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

    /// Load WAV / Ogg Vorbis / FLAC / cooked .fuseaudio (format sniffed from the bytes). The path
    /// is read through the VFS when a mount resolves it, else from the filesystem. Ogg clips longer
    /// than AudioDesc::stream_threshold_seconds stay encoded and play as streaming voices.
    Handle<AudioClip> load_clip(const char* path);
    Handle<AudioClip> load_clip_memory(const u8* data, usize size);
    const AudioClip* find_clip(Handle<AudioClip> handle) const { return m_clips.get(handle); }
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

    /// The most recently rendered block (interleaved stereo, `frames_per_buf` frames).
    const std::vector<float>& last_mix_buffer() const { return m_mixBuffer; }
    /// Blocks rendered since init (each one was handed to the output backend).
    u64 blocks_rendered() const { return m_blocksRendered; }

    /// Device output counters: underruns, drops, latency, reconnects (GAP-AUDIO-DEVICE-OUT).
    AudioOutputStats output_stats() const { return m_backend.output_stats(); }
    /// Null backend capture of everything the engine output (AudioDesc::capture_frames).
    const std::vector<float>& output_capture() const { return m_backend.capture(); }
    void clear_output_capture() { m_backend.clear_capture(); }
    /// Test hook: force the OpenAL device-lost / reconnect path.
    void simulate_output_device_loss() { m_backend.simulate_device_loss(); }

    /// Output backend (gain / position readback for headless verification).
    const AudioBackend& backend() const { return m_backend; }
    const ConvReverbCpu& cpu_reverb() const { return m_cpuReverb; }

    AudioBusMixer& bus_mixer() { return m_mixer.bus_mixer(); }
    const AudioBusMixer& bus_mixer() const { return m_mixer.bus_mixer(); }

    SpatialMixer& spatial_mixer() { return m_mixer; }
    const SpatialMixer& spatial_mixer() const { return m_mixer; }

    void set_occlusion_blockers(const AABB* blockers, u32 blocker_count) {
        m_mixer.set_occlusion_blockers(blockers, blocker_count);
    }
    void clear_occlusion_blockers() { m_mixer.clear_occlusion_blockers(); }

private:
    void apply_reverb_(std::vector<float>& stereo_buffer, u32 frames, const Vec3& listener_pos);
    void sync_backend_sources_(AudioRegistry& registry);
    void render_block_(AudioRegistry& registry, float dt);

    AudioDesc m_desc{};
    bool m_initialized = false;
    AudioBackend m_backend;
    HandleMap<AudioClip> m_clips;
    std::vector<ReverbZone> m_reverbZones;
    SpatialMixer m_mixer;
    ConvReverbCpu m_cpuReverb;
    std::vector<float> m_mixBuffer;
    std::vector<float> m_dryBuffer;
    std::vector<float> m_wetBuffer;
    std::unordered_map<EntityId, u32> m_entitySources;
    u64 m_blocksRendered = 0;
};

} // namespace fuse::audio
