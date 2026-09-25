#include <fuse/audio/audio_engine.hpp>

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/occlusion.hpp>
#include <fuse/audio/reverb_zones.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

void AudioEngine::init(const AudioDesc& desc) {
    destroy();
    m_desc = desc;
    m_backend.init(desc);
    m_mixer.configure(desc.sample_rate, desc.max_sources, desc.hrtf_enabled);
    m_mixBuffer.clear();
    m_dryBuffer.clear();
    m_reverbZones.clear();
    m_initialized = m_backend.is_initialized();
}

void AudioEngine::destroy() {
    m_backend.shutdown();
    m_entitySources.clear();
    m_clips = HandleMap<AudioClip>{};
    m_reverbZones.clear();
    m_mixBuffer.clear();
    m_dryBuffer.clear();
    m_wetBuffer.clear();
    m_mixer.clear_one_shots();
    m_cpuReverb.reset();
    m_initialized = false;
}

void AudioEngine::update(AudioRegistry& registry, float dt) {
    if (!m_initialized) {
        return;
    }

    const u32 frames = m_desc.frames_per_buf;
    m_mixer.mix(registry, m_clips, dt, m_mixBuffer, frames);

    const AudioListener* listener = registry.listener();
    const Vec3 listener_pos = listener != nullptr ? listener->position : Vec3{};
    apply_reverb_(m_mixBuffer, frames, listener_pos);
    sync_backend_sources_(registry);
    if (listener != nullptr) {
        m_backend.set_listener_position(listener->position.x, listener->position.y,
                                      listener->position.z);
        m_backend.set_listener_orientation(listener->forward.x, listener->forward.y,
                                             listener->forward.z, listener->up.x, listener->up.y,
                                             listener->up.z);
    }

    // Play heads follow the sample clock (frames rendered), not the caller's dt, so consecutive
    // buffers are sample-continuous.
    for (u32 retired : m_mixer.advance(registry, m_clips, frames)) {
        m_backend.destroy_source(retired);
    }
}

void AudioEngine::play_at(Handle<AudioClip> clip, const Vec3& position, float volume, float pitch) {
    if (!m_initialized || !m_clips.valid(clip)) {
        return;
    }

    OneShotVoice voice;
    voice.clip = clip;
    voice.position = position;
    voice.volume = volume;
    voice.pitch = pitch;
    voice.spatial = true;
    voice.backend_source = m_backend.create_source();
    m_backend.set_source_position(voice.backend_source, position.x, position.y, position.z);
    m_backend.set_source_gain(voice.backend_source, volume);
    m_backend.play_source(voice.backend_source);
    m_mixer.add_one_shot(voice);
}

void AudioEngine::play_2d(Handle<AudioClip> clip, float volume) {
    if (!m_initialized || !m_clips.valid(clip)) {
        return;
    }

    OneShotVoice voice;
    voice.clip = clip;
    voice.volume = volume;
    voice.spatial = false;
    voice.backend_source = m_backend.create_source();
    m_backend.set_source_gain(voice.backend_source, volume);
    m_backend.play_source(voice.backend_source);
    m_mixer.add_one_shot(voice);
}

Handle<AudioClip> AudioEngine::load_clip(const char* path) {
    AudioClip clip;
    if (!clip.load_wav(path)) {
        return Handle<AudioClip>::invalid();
    }
    return register_clip(std::move(clip));
}

Handle<AudioClip> AudioEngine::register_clip(AudioClip&& clip) {
    if (clip.empty()) {
        return Handle<AudioClip>::invalid();
    }
    return m_clips.insert(std::move(clip));
}

void AudioEngine::unload_clip(Handle<AudioClip> handle) {
    m_clips.remove(handle);
}

void AudioEngine::add_reverb_zone(const ReverbZone& zone) {
    if (m_reverbZones.size() >= m_desc.max_reverb_zones) {
        return;
    }
    m_reverbZones.push_back(zone);

    const AudioClip* ir = m_clips.get(zone.impulse_response);
    if (ir != nullptr && !ir->samples.empty()) {
        const float* mono = ir->samples.data();
        m_cpuReverb.init(mono, ir->frame_count(), m_desc.frames_per_buf);
    }
}

void AudioEngine::clear_reverb_zones() {
    m_reverbZones.clear();
    m_cpuReverb.reset();
}

void AudioEngine::set_reverb_send_level(float send_level) {
    const float clamped = std::clamp(send_level, 0.f, 1.f);
    for (ReverbZone& zone : m_reverbZones) {
        zone.send_level = clamped;
    }
}

float AudioEngine::reverb_send_level() const {
    if (m_reverbZones.empty()) {
        return 0.f;
    }
    return m_reverbZones.front().send_level;
}

void AudioEngine::apply_reverb_(std::vector<float>& stereo_buffer, u32 frames,
                                const Vec3& listener_pos) {
    if (m_reverbZones.empty() || m_cpuReverb.ir_length() == 0) {
        return;
    }

    std::vector<ReverbZoneParams> zone_params;
    zone_params.reserve(m_reverbZones.size());
    for (const ReverbZone& zone : m_reverbZones) {
        zone_params.push_back({zone.bounds, zone.wet_dry, zone.send_level});
    }

    const ReverbZoneBlend blend =
        blend_reverb_zones(listener_pos, zone_params.data(),
                           static_cast<u32>(zone_params.size()));
    if (blend.active_zone_count == 0) {
        return;
    }

    // Mono reverb send (L+R)/2 convolved once; the wet signal is added to both channels while
    // the dry path keeps its stereo image.
    m_dryBuffer.resize(frames);
    m_wetBuffer.assign(frames, 0.f);
    const float wet_mix = compute_effective_wet_mix(blend);

    for (u32 frame = 0; frame < frames; ++frame) {
        m_dryBuffer[frame] = 0.5f * (stereo_buffer[static_cast<usize>(frame) * 2]
                                       + stereo_buffer[static_cast<usize>(frame) * 2 + 1]);
    }

    m_cpuReverb.process(m_dryBuffer.data(), m_wetBuffer.data(), frames);

    for (u32 frame = 0; frame < frames; ++frame) {
        for (u32 ch = 0; ch < 2; ++ch) {
            float& sample = stereo_buffer[static_cast<usize>(frame) * 2 + ch];
            sample = blend_dry_wet_sample(sample, m_wetBuffer[frame], wet_mix);
        }
    }
}

void AudioEngine::sync_backend_sources_(AudioRegistry& registry) {
    // Release backend sources whose entity was destroyed since the last update.
    for (auto it = m_entitySources.begin(); it != m_entitySources.end();) {
        if (registry.find_source(it->first) == nullptr) {
            m_backend.destroy_source(it->second);
            it = m_entitySources.erase(it);
        } else {
            ++it;
        }
    }

    for (EntityId entity : registry.source_entities()) {
        AudioSource* source = registry.find_source(entity);
        if (source == nullptr) {
            continue;
        }

        if (source->backend_source == 0) {
            source->backend_source = m_backend.create_source();
            m_entitySources[entity] = source->backend_source;
        }

        const AttenuationParams attenuation_params = make_attenuation_params(source->desc);

        const AudioListener* listener = registry.listener();
        const float distance = listener != nullptr
            ? source->position.distance(listener->position)
            : 0.f;
        const float distance_attenuation = source->desc.spatial && listener != nullptr
            ? compute_attenuation(distance, attenuation_params)
            : 1.f;
        const float visibility = listener != nullptr
            ? m_mixer.compute_source_visibility(listener->position, source->position,
                                                source->desc.occlusion)
            : std::clamp(source->desc.occlusion, 0.f, 1.f);
        const OcclusionAttenuation occlusion = evaluate_occlusion_attenuation(visibility);
        const float gain =
            source->desc.volume * distance_attenuation * occlusion.gain * occlusion.hf_gain;
        m_backend.set_source_gain(source->backend_source, gain);
        m_backend.set_source_position(source->backend_source, source->position.x,
                                      source->position.y, source->position.z);

        if (source->playing && !source->paused) {
            m_backend.play_source(source->backend_source);
        } else if (source->paused) {
            m_backend.pause_source(source->backend_source, true);
        } else {
            m_backend.stop_source(source->backend_source);
        }
    }
}

} // namespace fuse::audio
