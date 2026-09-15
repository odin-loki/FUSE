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

bool is_valid_audio_bus(AudioBus bus) {
    u32 index = 0;
    return try_bus_index(bus, index);
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
    const float listener_gain = clamp_bus_gain(listener_master_volume);
    if (!is_valid_audio_bus(bus)) {
        return listener_gain;
    }
    return listener_gain * effective_gain(bus);
}

bool AudioBusMixer::should_apply_bus_gain(AudioBus bus) const {
    if (!is_valid_audio_bus(bus)) {
        return false;
    }
    return effective_gain(bus) > kBusMuteEpsilon;
}

void AudioBusMixer::reset_gains() {
    for (u32 i = 0; i < static_cast<u32>(AudioBus::Count); ++i) {
        m_gains[i] = 1.f;
    }
    m_parents[static_cast<u32>(AudioBus::Master)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Sfx)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Music)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Voice)] = AudioBus::Master;
}

float compute_effective_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                                    float listener_master_volume) {
    return mixer.effective_output_gain(bus, listener_master_volume);
}

bool is_bus_muted(const AudioBusMixer& mixer, AudioBus bus) {
    if (!is_valid_audio_bus(bus)) {
        return false;
    }
    return mixer.effective_gain(bus) <= kBusMuteEpsilon;
}

} // namespace fuse::audio
