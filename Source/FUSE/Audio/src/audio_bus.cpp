#include <fuse/audio/audio_bus.hpp>

#include <cmath>

namespace fuse::audio {

namespace {

constexpr float kBusMuteEpsilon = 1e-6f;

bool try_bus_index(AudioBus bus, u32& out_index) {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return false;
    }
    out_index = index;
    return true;
}

} // namespace

float clamp_bus_gain(float gain) {
    return std::clamp(gain, 0.f, 1.f);
}

float clamp_listener_master_volume(float volume) {
    return clamp_bus_gain(volume);
}

bool is_near_zero_bus_gain(float gain) {
    return clamp_bus_gain(gain) <= kBusMuteEpsilon;
}

bool is_listener_master_muted(float listener_master_volume) {
    return is_near_zero_bus_gain(listener_master_volume);
}

bool should_skip_listener_master_mix(float listener_master_volume) {
    return is_listener_master_muted(listener_master_volume);
}

bool is_audible_bus_gain(float gain) {
    return !is_near_zero_bus_gain(gain);
}

bool is_valid_audio_bus(AudioBus bus) {
    u32 index = 0;
    return try_bus_index(bus, index);
}

bool is_empty_audio_bus(AudioBus bus) {
    return !is_valid_audio_bus(bus);
}

bool is_category_audio_bus(AudioBus bus) {
    return is_valid_audio_bus(bus) && bus != AudioBus::Master;
}

AudioBusMixer::AudioBusMixer() {
    m_parents[static_cast<u32>(AudioBus::Master)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Sfx)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Music)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Voice)] = AudioBus::Master;
}

void AudioBusMixer::set_bus_gain(AudioBus bus, float gain) {
    u32 index = 0;
    if (!try_bus_index(bus, index)) {
        return;
    }
    m_gains[index] = clamp_bus_gain(gain);
}

float AudioBusMixer::bus_gain(AudioBus bus) const {
    u32 index = 0;
    if (!try_bus_index(bus, index)) {
        return 1.f;
    }
    return m_gains[index];
}

void AudioBusMixer::set_bus_parent(AudioBus bus, AudioBus parent) {
    u32 index = 0;
    if (!try_bus_index(bus, index)) {
        return;
    }
    if (bus == AudioBus::Master) {
        m_parents[index] = AudioBus::Master;
        return;
    }
    m_parents[index] = parent;
}

AudioBus AudioBusMixer::bus_parent(AudioBus bus) const {
    u32 index = 0;
    if (!try_bus_index(bus, index)) {
        return AudioBus::Master;
    }
    return m_parents[index];
}

float AudioBusMixer::routed_bus_gain(AudioBus bus) const {
    if (!is_valid_audio_bus(bus)) {
        return 1.f;
    }
    if (bus == AudioBus::Master) {
        return 1.f;
    }

    float gain = bus_gain(bus);
    AudioBus current = bus_parent(bus);
    u32 hops = 0;
    while (current != AudioBus::Master) {
        gain *= bus_gain(current);
        current = bus_parent(current);
        ++hops;
        if (hops >= static_cast<u32>(AudioBus::Count)) {
            break;
        }
    }
    return gain;
}

float AudioBusMixer::effective_gain(AudioBus bus) const {
    if (!is_valid_audio_bus(bus)) {
        return 1.f;
    }
    if (bus == AudioBus::Master) {
        return bus_gain(AudioBus::Master);
    }
    return routed_bus_gain(bus) * bus_gain(AudioBus::Master);
}

float AudioBusMixer::effective_output_gain(AudioBus bus, float listener_master_volume) const {
    const float listener_gain = clamp_listener_master_volume(listener_master_volume);
    if (!is_valid_audio_bus(bus)) {
        return listener_gain;
    }
    return listener_gain * effective_gain(bus);
}

void AudioBusMixer::set_bus_muted(AudioBus bus, bool muted) {
    u32 index = 0;
    if (!try_bus_index(bus, index) || bus == AudioBus::Master) {
        return;
    }
    m_muted[index] = muted;
}

bool AudioBusMixer::bus_muted(AudioBus bus) const {
    u32 index = 0;
    if (!try_bus_index(bus, index) || bus == AudioBus::Master) {
        return false;
    }
    return m_muted[index];
}

bool AudioBusMixer::any_bus_muted() const {
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        if (static_cast<AudioBus>(i) == AudioBus::Master) {
            continue;
        }
        if (m_muted[i]) {
            return true;
        }
    }
    return false;
}

u32 AudioBusMixer::muted_bus_count() const {
    u32 count = 0;
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        if (static_cast<AudioBus>(i) == AudioBus::Master) {
            continue;
        }
        if (m_muted[i]) {
            ++count;
        }
    }
    return count;
}

void AudioBusMixer::clear_bus_mute() {
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        m_muted[i] = false;
    }
}

void AudioBusMixer::clear_bus_mute(AudioBus bus) {
    u32 index = 0;
    if (!try_bus_index(bus, index) || bus == AudioBus::Master) {
        return;
    }
    m_muted[index] = false;
}

void AudioBusMixer::reset_mute_and_solo() {
    clear_bus_mute();
    clear_bus_solo();
}

void AudioBusMixer::set_bus_solo(AudioBus bus, bool solo) {
    u32 index = 0;
    if (!try_bus_index(bus, index) || bus == AudioBus::Master) {
        return;
    }
    m_solo[index] = solo;
}

bool AudioBusMixer::bus_soloed(AudioBus bus) const {
    u32 index = 0;
    if (!try_bus_index(bus, index) || bus == AudioBus::Master) {
        return false;
    }
    return m_solo[index];
}

bool AudioBusMixer::any_bus_soloed() const {
    return soloed_bus_count() > 0;
}

u32 AudioBusMixer::soloed_bus_count() const {
    u32 count = 0;
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        if (static_cast<AudioBus>(i) == AudioBus::Master) {
            continue;
        }
        if (m_solo[i]) {
            ++count;
        }
    }
    return count;
}

void AudioBusMixer::clear_bus_solo() {
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        m_solo[i] = false;
    }
}

bool AudioBusMixer::is_any_ancestor_bus_soloed(AudioBus bus) const {
    if (!is_valid_audio_bus(bus) || bus == AudioBus::Master) {
        return false;
    }

    AudioBus current = bus_parent(bus);
    u32 hops = 0;
    while (current != AudioBus::Master) {
        if (bus_soloed(current)) {
            return true;
        }
        current = bus_parent(current);
        ++hops;
        if (hops >= static_cast<u32>(AudioBus::Count)) {
            break;
        }
    }
    return false;
}

bool AudioBusMixer::is_bus_solo_audible(AudioBus bus) const {
    if (!is_valid_audio_bus(bus) || bus == AudioBus::Master) {
        return false;
    }
    return bus_soloed(bus) || is_any_ancestor_bus_soloed(bus);
}

bool AudioBusMixer::is_bus_solo_silenced(AudioBus bus) const {
    if (!any_bus_soloed()) {
        return false;
    }
    if (bus == AudioBus::Master) {
        return false;
    }
    if (!is_valid_audio_bus(bus)) {
        return false;
    }
    return !is_bus_solo_audible(bus);
}

bool AudioBusMixer::is_parent_chain_muted(AudioBus bus) const {
    if (!is_valid_audio_bus(bus) || bus == AudioBus::Master) {
        return false;
    }

    AudioBus current = bus_parent(bus);
    u32 hops = 0;
    while (current != AudioBus::Master) {
        if (bus_muted(current)) {
            return true;
        }
        current = bus_parent(current);
        ++hops;
        if (hops >= static_cast<u32>(AudioBus::Count)) {
            break;
        }
    }
    return false;
}

bool AudioBusMixer::should_apply_bus_gain(AudioBus bus) const {
    if (!is_valid_audio_bus(bus)) {
        return false;
    }
    if (bus != AudioBus::Master && bus_muted(bus)) {
        return false;
    }
    if (bus != AudioBus::Master && is_parent_chain_muted(bus)) {
        return false;
    }
    return is_audible_bus_gain(effective_gain(bus));
}

bool AudioBusMixer::should_mix_bus(AudioBus bus) const {
    if (is_empty_audio_bus(bus)) {
        return false;
    }
    if (!should_apply_bus_gain(bus)) {
        return false;
    }
    if (is_bus_solo_silenced(bus)) {
        return false;
    }
    return true;
}

bool AudioBusMixer::should_mix_bus_with_listener(AudioBus bus,
                                                 float listener_master_volume) const {
    if (should_skip_listener_master_mix(listener_master_volume)) {
        return false;
    }
    return should_mix_bus(bus);
}

void AudioBusMixer::reset_gains() {
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        m_gains[i] = 1.f;
        m_muted[i] = false;
        m_solo[i] = false;
    }
    m_parents[static_cast<u32>(AudioBus::Master)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Sfx)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Music)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Voice)] = AudioBus::Master;
}

float compute_effective_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                                    float listener_master_volume) {
    const float listener_gain = clamp_listener_master_volume(listener_master_volume);
    if (is_empty_audio_bus(bus)) {
        return listener_gain;
    }
    return mixer.effective_output_gain(bus, listener_master_volume);
}

float compute_mix_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                              float listener_master_volume) {
    if (is_empty_audio_bus(bus)) {
        return 0.f;
    }
    if (should_skip_bus_mix(mixer, bus, listener_master_volume)) {
        return 0.f;
    }
    return compute_effective_output_gain(mixer, bus, listener_master_volume);
}

bool should_skip_bus_mix(const AudioBusMixer& mixer, AudioBus bus) {
    return !mixer.should_mix_bus(bus);
}

bool should_skip_bus_mix(const AudioBusMixer& mixer, AudioBus bus,
                         float listener_master_volume) {
    if (should_skip_listener_master_mix(listener_master_volume)) {
        return true;
    }
    return should_skip_bus_mix(mixer, bus);
}

bool is_bus_mix_silenced(const AudioBusMixer& mixer, AudioBus bus) {
    return should_skip_bus_mix(mixer, bus);
}

bool is_bus_mix_silenced(const AudioBusMixer& mixer, AudioBus bus,
                         float listener_master_volume) {
    return should_skip_bus_mix(mixer, bus, listener_master_volume);
}

bool has_any_mixable_bus(const AudioBusMixer& mixer, float listener_master_volume) {
    if (should_skip_listener_master_mix(listener_master_volume)) {
        return false;
    }
    if (is_master_bus_muted(mixer)) {
        return false;
    }
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        const AudioBus bus = static_cast<AudioBus>(i);
        if (!is_category_audio_bus(bus)) {
            continue;
        }
        if (mixer.should_mix_bus(bus)) {
            return true;
        }
    }
    return false;
}

bool is_master_bus_muted(const AudioBusMixer& mixer) {
    return !is_audible_bus_gain(mixer.bus_gain(AudioBus::Master));
}

bool is_bus_parent_chain_muted(const AudioBusMixer& mixer, AudioBus bus) {
    if (!is_valid_audio_bus(bus) || bus == AudioBus::Master) {
        return false;
    }
    return mixer.bus_muted(bus) || mixer.is_parent_chain_muted(bus);
}

bool is_bus_muted(const AudioBusMixer& mixer, AudioBus bus) {
    if (!is_valid_audio_bus(bus)) {
        return false;
    }
    if (is_bus_parent_chain_muted(mixer, bus)) {
        return true;
    }
    return !is_audible_bus_gain(mixer.effective_gain(bus));
}

} // namespace fuse::audio
