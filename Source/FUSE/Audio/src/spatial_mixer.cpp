#include <fuse/audio/spatial_mixer.hpp>

#include <fuse/audio/attenuation.hpp>
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

float SpatialMixer::sample_clip(const AudioClip& clip, float play_head, u32 channel) const {
    if (clip.channel_count == 0 || clip.samples.empty()) {
        return 0.f;
    }

    const u32 frame = static_cast<u32>(play_head);
    if (frame >= clip.frame_count()) {
        return 0.f;
    }

    const u32 ch = std::min(channel, clip.channel_count - 1);
    return clip.samples[static_cast<usize>(frame) * clip.channel_count + ch];
}

void SpatialMixer::apply_hrtf_pan(float mono_sample, const Vec3& rel, float attenuation,
                                  float& left, float& right) const {
    const float distance = rel.length();
    if (!m_hrtfEnabled || distance < 1e-5f) {
        left += mono_sample * attenuation;
        right += mono_sample * attenuation;
        return;
    }

    const float azimuth = std::atan2(rel.x, -rel.z);
    const float pan = std::sin(azimuth);
    const float left_gain = std::sqrt(0.5f * (1.f - pan));
    const float right_gain = std::sqrt(0.5f * (1.f + pan));
    left += mono_sample * attenuation * left_gain;
    right += mono_sample * attenuation * right_gain;
}

void SpatialMixer::mix(const AudioRegistry& registry, const HandleMap<AudioClip>& clips, float dt,
                       std::vector<float>& stereo_out, u32 frames) {
    stereo_out.assign(static_cast<usize>(frames) * 2, 0.f);
    m_lastMasterGain = 1.f;

    const AudioListener* listener = registry.listener();
    const Vec3 listener_pos = listener != nullptr ? listener->position : Vec3{};
    ListenerBasis basis{};
    if (listener != nullptr) {
        m_lastMasterGain = listener->master_volume;
        basis = make_listener_basis(listener->forward, listener->up);
    }

    u32 active_sources = 0;
    for (EntityId entity : registry.source_entities()) {
        const AudioSource* source = registry.find_source(entity);
        if (source == nullptr || !source->playing || source->paused) {
            continue;
        }
        if (!clips.valid(source->desc.clip)) {
            continue;
        }
        if (active_sources >= m_maxSources) {
            break;
        }
        ++active_sources;

        const AudioClip* clip = clips.get(source->desc.clip);
        if (clip == nullptr || clip->empty()) {
            continue;
        }

        AttenuationParams attenuation_params;
        attenuation_params.curve = source->desc.attenuation;
        attenuation_params.min_dist = source->desc.min_distance;
        attenuation_params.max_dist = source->desc.max_distance;
        attenuation_params.rolloff = source->desc.rolloff;

        const float distance = source->position.distance(listener_pos);
        const float distance_attenuation = source->desc.spatial
            ? compute_attenuation(distance, attenuation_params)
            : 1.f;

        float visibility = std::clamp(source->desc.occlusion, 0.f, 1.f);
        if (!m_occlusionBlockers.empty()) {
            visibility *= compute_blockers_visibility(listener_pos, source->position,
                                                      m_occlusionBlockers.data(),
                                                      static_cast<u32>(m_occlusionBlockers.size()));
        }
        const OcclusionAttenuation occlusion = evaluate_occlusion_attenuation(visibility);
        const float effective_attenuation =
            distance_attenuation * occlusion.gain * occlusion.hf_gain;

        const Vec3 world_rel = source->position - listener_pos;
        const Vec3 rel = listener != nullptr ? to_listener_space(world_rel, basis) : world_rel;
        const float bus_gain = m_busMixer.effective_gain(source->desc.bus);

        for (u32 frame = 0; frame < frames; ++frame) {
            const float local_t = source->play_head + static_cast<float>(frame) / static_cast<float>(m_sampleRate);
            float mono = 0.f;
            if (clip->channel_count == 1) {
                mono = sample_clip(*clip, local_t * clip->sample_rate, 0);
            } else {
                mono = 0.5f * (sample_clip(*clip, local_t * clip->sample_rate, 0)
                               + sample_clip(*clip, local_t * clip->sample_rate, 1));
            }
            mono *= source->desc.volume * source->desc.pitch;

            float left = 0.f;
            float right = 0.f;
            if (source->desc.spatial) {
                apply_hrtf_pan(mono, rel, effective_attenuation, left, right);
            } else {
                left = mono;
                right = mono;
            }

            const float master = m_lastMasterGain * bus_gain;
            stereo_out[static_cast<usize>(frame) * 2] += left * master;
            stereo_out[static_cast<usize>(frame) * 2 + 1] += right * master;
        }
    }
}

} // namespace fuse::audio
