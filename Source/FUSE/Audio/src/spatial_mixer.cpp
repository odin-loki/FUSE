#include <fuse/audio/spatial_mixer.hpp>

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/binaural_pan.hpp>
#include <fuse/audio/occlusion.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

void SpatialMixer::configure(u32 sample_rate, u32 max_sources, bool hrtf_enabled) {
    m_sampleRate = sample_rate;
    m_maxSources = max_sources;
    m_hrtfEnabled = hrtf_enabled;
}

void SpatialMixer::set_occlusion_blockers(const AABB* blockers, u32 blocker_count) {
    m_occlusionBlockers.clear();
    if (blockers == nullptr || blocker_count == 0) {
        return;
    }
    m_occlusionBlockers.assign(blockers, blockers + blocker_count);
}

void SpatialMixer::clear_occlusion_blockers() {
    m_occlusionBlockers.clear();
}

float SpatialMixer::compute_source_visibility(const Vec3& listener, const Vec3& source,
                                              float source_occlusion) const {
    if (m_occlusionBlockers.empty()) {
        return std::clamp(source_occlusion, 0.f, 1.f);
    }
    return compute_effective_visibility(listener, source, source_occlusion,
                                        m_occlusionBlockers.data(),
                                        static_cast<u32>(m_occlusionBlockers.size()));
}

OcclusionAttenuation SpatialMixer::compute_source_occlusion_attenuation(
    const Vec3& listener, const Vec3& source, float source_occlusion) const {
    if (m_occlusionBlockers.empty()) {
        return evaluate_occlusion_attenuation(std::clamp(source_occlusion, 0.f, 1.f));
    }
    return evaluate_occlusion_from_blockers(listener, source, source_occlusion,
                                            m_occlusionBlockers.data(),
                                            static_cast<u32>(m_occlusionBlockers.size()));
}

BinauralPanGains SpatialMixer::compute_source_binaural_pan_gains(const Vec3& rel_listener,
                                                                 float distance_attenuation,
                                                                 float occlusion_gain) const {
    BinauralPanGains pan = compute_binaural_pan_gains_guarded(m_hrtfEnabled, rel_listener);
    if (should_apply_hrtf_pan(m_hrtfEnabled, rel_listener)) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain);
    }
    return pan;
}

float SpatialMixer::sample_clip(const AudioClip& clip, double frame_pos, u32 channel,
                                bool looping) const {
    const u32 frame_count = clip.frame_count();
    if (clip.channel_count == 0 || frame_count == 0 || frame_pos < 0.0) {
        return 0.f;
    }

    const u32 i0 = static_cast<u32>(frame_pos);
    if (i0 >= frame_count) {
        return 0.f;
    }
    u32 i1 = i0 + 1;
    if (i1 >= frame_count) {
        i1 = looping ? 0 : frame_count - 1;
    }
    const float frac = static_cast<float>(frame_pos - static_cast<double>(i0));
    const u32 ch = std::min(channel, clip.channel_count - 1);
    const float a = clip.samples[static_cast<usize>(i0) * clip.channel_count + ch];
    if (frac <= 0.f) {
        return a;
    }
    const float b = clip.samples[static_cast<usize>(i1) * clip.channel_count + ch];
    return a + (b - a) * frac;
}

u32 SpatialMixer::add_one_shot(const OneShotVoice& voice) {
    OneShotVoice queued = voice;
    queued.id = m_nextOneShotId++;
    if (m_nextOneShotId == 0) {
        m_nextOneShotId = 1;
    }
    m_oneShots.push_back(queued);
    return queued.id;
}

void SpatialMixer::render_voice(const VoiceRequest& voice, const Vec3& listener_pos,
                                bool has_listener, const ListenerBasis& basis,
                                std::vector<float>& stereo_out, u32 frames) {
    const AudioClip& clip = *voice.clip;
    const u32 clip_frames = clip.frame_count();
    if (clip_frames == 0 || clip.sample_rate == 0 || m_sampleRate == 0) {
        return;
    }

    const float output_gain = m_busMixer.effective_output_gain(voice.bus, m_lastMasterGain);

    float target_left = voice.volume * output_gain;
    float target_right = target_left;
    float hf_gain = 1.f;
    if (voice.spatial) {
        const float distance = voice.position.distance(listener_pos);
        const float distance_attenuation = compute_attenuation(distance, voice.attenuation);
        const OcclusionAttenuation occlusion =
            compute_source_occlusion_attenuation(listener_pos, voice.position, voice.occlusion);
        const Vec3 world_rel = voice.position - listener_pos;
        const Vec3 rel = has_listener ? to_listener_space(world_rel, basis) : world_rel;
        BinauralPanGains pan{};
        if (should_apply_hrtf_pan(m_hrtfEnabled, rel)) {
            pan = compute_source_binaural_pan_gains(rel, distance_attenuation, occlusion.gain);
        }
        const float scalar = target_left * distance_attenuation * occlusion.gain;
        target_left = scalar * pan.left;
        target_right = scalar * pan.right;
        hf_gain = std::clamp(occlusion.hf_gain, 0.f, 1.f);
    }

    VoiceState& state = m_voiceState[voice.key];
    if (!state.primed) {
        state.gain_left = target_left;
        state.gain_right = target_right;
        state.primed = true;
    }
    state.touched = true;
    const float start_left = state.gain_left;
    const float start_right = state.gain_right;

    const double clip_rate = static_cast<double>(clip.sample_rate);
    const double step =
        static_cast<double>(std::max(voice.pitch, 0.f)) * clip_rate / static_cast<double>(m_sampleRate);
    const double start = voice.play_head * clip_rate;
    const double length = static_cast<double>(clip_frames);
    const float lp_coeff = 1.f
        - std::exp(-2.f * 3.14159265358979323846f * kOcclusionCrossoverHz
                   / static_cast<float>(m_sampleRate));
    const bool stereo_passthrough = !voice.spatial && clip.channel_count >= 2;

    for (u32 frame = 0; frame < frames; ++frame) {
        double pos = start + static_cast<double>(frame) * step;
        if (voice.looping) {
            pos = std::fmod(pos, length);
        } else if (pos >= length) {
            break;
        }

        float in_left = 0.f;
        float in_right = 0.f;
        if (clip.channel_count == 1) {
            in_left = sample_clip(clip, pos, 0, voice.looping);
            in_right = in_left;
        } else if (stereo_passthrough) {
            in_left = sample_clip(clip, pos, 0, voice.looping);
            in_right = sample_clip(clip, pos, 1, voice.looping);
        } else {
            in_left = 0.5f * (sample_clip(clip, pos, 0, voice.looping)
                              + sample_clip(clip, pos, 1, voice.looping));
            in_right = in_left;
        }

        if (voice.spatial) {
            // High shelf: LF passes, content above the crossover is scaled by hf_gain.
            state.lp_left += lp_coeff * (in_left - state.lp_left);
            if (hf_gain < 1.f) {
                in_left = state.lp_left + hf_gain * (in_left - state.lp_left);
            }
            in_right = in_left;
        }

        float gain_left = target_left;
        float gain_right = target_right;
        if (frame < kGainRampFrames) {
            const float t = static_cast<float>(frame + 1) / static_cast<float>(kGainRampFrames);
            gain_left = start_left + (target_left - start_left) * t;
            gain_right = start_right + (target_right - start_right) * t;
        }

        stereo_out[static_cast<usize>(frame) * 2] += in_left * gain_left;
        stereo_out[static_cast<usize>(frame) * 2 + 1] += in_right * gain_right;
    }

    state.gain_left = target_left;
    state.gain_right = target_right;
}

void SpatialMixer::mix(const AudioRegistry& registry, const HandleMap<AudioClip>& clips, float dt,
                       std::vector<float>& stereo_out, u32 frames) {
    (void)dt; // Playback follows the sample clock; see advance().
    stereo_out.assign(static_cast<usize>(frames) * 2, 0.f);
    m_lastMasterGain = 1.f;
    m_lastMixedVoices = 0;
    m_lastCulledVoices = 0;

    const AudioListener* listener = registry.listener();
    const Vec3 listener_pos = listener != nullptr ? listener->position : Vec3{};
    ListenerBasis basis{};
    if (listener != nullptr) {
        m_lastMasterGain = listener->master_volume;
        basis = make_listener_basis_safe(listener->forward, listener->up);
    }

    std::vector<VoiceRequest> requests;
    requests.reserve(registry.source_entities().size() + m_oneShots.size());

    auto estimate_audibility = [&](VoiceRequest& request) {
        float gain = request.volume * m_busMixer.effective_output_gain(request.bus, m_lastMasterGain);
        if (request.spatial) {
            const float distance = request.position.distance(listener_pos);
            gain *= compute_attenuation(distance, request.attenuation);
            gain *= compute_source_occlusion_attenuation(listener_pos, request.position,
                                                         request.occlusion)
                        .gain;
        }
        request.audibility = gain;
    };

    for (EntityId entity : registry.source_entities()) {
        const AudioSource* source = registry.find_source(entity);
        if (source == nullptr || !source->playing || source->paused) {
            continue;
        }
        const AudioClip* clip = clips.get(source->desc.clip);
        if (clip == nullptr || clip->empty()) {
            continue;
        }

        VoiceRequest request;
        request.key = static_cast<u64>(entity);
        request.clip = clip;
        request.position = source->position;
        request.volume = source->desc.volume;
        request.pitch = source->desc.pitch;
        request.spatial = source->desc.spatial;
        request.looping = source->desc.looping;
        request.play_head = source->play_head;
        request.attenuation = make_attenuation_params(source->desc);
        request.occlusion = source->desc.occlusion;
        request.bus = source->desc.bus;
        estimate_audibility(request);
        requests.push_back(request);
    }

    for (const OneShotVoice& voice : m_oneShots) {
        const AudioClip* clip = clips.get(voice.clip);
        if (clip == nullptr || clip->empty()) {
            continue;
        }
        VoiceRequest request;
        request.key = (static_cast<u64>(1) << 32) | static_cast<u64>(voice.id);
        request.clip = clip;
        request.position = voice.position;
        request.volume = voice.volume;
        request.pitch = voice.pitch;
        request.spatial = voice.spatial;
        request.play_head = voice.play_head;
        request.bus = voice.bus;
        estimate_audibility(request);
        requests.push_back(request);
    }

    if (requests.size() > m_maxSources) {
        std::stable_sort(requests.begin(), requests.end(),
                         [](const VoiceRequest& a, const VoiceRequest& b) {
                             return a.audibility > b.audibility;
                         });
        m_lastCulledVoices = static_cast<u32>(requests.size()) - m_maxSources;
        requests.resize(m_maxSources);
    }

    for (const VoiceRequest& request : requests) {
        render_voice(request, listener_pos, listener != nullptr, basis, stereo_out, frames);
    }
    m_lastMixedVoices = static_cast<u32>(requests.size());

    for (auto it = m_voiceState.begin(); it != m_voiceState.end();) {
        if (!it->second.touched) {
            it = m_voiceState.erase(it);
        } else {
            it->second.touched = false;
            ++it;
        }
    }
}

std::vector<u32> SpatialMixer::advance(AudioRegistry& registry, const HandleMap<AudioClip>& clips,
                                       u32 frames) {
    std::vector<u32> retired;
    if (m_sampleRate == 0) {
        return retired;
    }
    const double elapsed = static_cast<double>(frames) / static_cast<double>(m_sampleRate);

    for (EntityId entity : registry.source_entities()) {
        AudioSource* source = registry.find_source(entity);
        if (source == nullptr || !source->playing || source->paused) {
            continue;
        }
        const AudioClip* clip = clips.get(source->desc.clip);
        if (clip == nullptr || clip->sample_rate == 0 || clip->frame_count() == 0) {
            continue;
        }

        const double length =
            static_cast<double>(clip->frame_count()) / static_cast<double>(clip->sample_rate);
        source->play_head += elapsed * static_cast<double>(std::max(source->desc.pitch, 0.f));
        if (source->play_head >= length) {
            if (source->desc.looping) {
                source->play_head = std::fmod(source->play_head, length);
            } else {
                source->playing = false;
                source->play_head = 0.0;
            }
        }
    }

    for (auto it = m_oneShots.begin(); it != m_oneShots.end();) {
        const AudioClip* clip = clips.get(it->clip);
        bool finished = clip == nullptr || clip->sample_rate == 0 || clip->frame_count() == 0;
        if (!finished) {
            const double length =
                static_cast<double>(clip->frame_count()) / static_cast<double>(clip->sample_rate);
            it->play_head += elapsed * static_cast<double>(std::max(it->pitch, 0.f));
            finished = it->play_head >= length;
        }
        if (finished) {
            if (it->backend_source != 0) {
                retired.push_back(it->backend_source);
            }
            it = m_oneShots.erase(it);
        } else {
            ++it;
        }
    }
    return retired;
}

} // namespace fuse::audio
