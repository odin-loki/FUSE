#include <fuse/audio/audio_bus.hpp>

namespace fuse::audio {

float clamp_bus_gain(float gain) {
    return std::clamp(gain, 0.f, 1.f);
}

AudioBusMixer::AudioBusMixer() {
    m_parents[static_cast<u32>(AudioBus::Master)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Sfx)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Music)] = AudioBus::Master;
    m_parents[static_cast<u32>(AudioBus::Voice)] = AudioBus::Master;
}

void AudioBusMixer::set_bus_gain(AudioBus bus, float gain) {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return;
    }
    m_gains[index] = clamp_bus_gain(gain);
}

float AudioBusMixer::bus_gain(AudioBus bus) const {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return 1.f;
    }
    return m_gains[index];
}

void AudioBusMixer::set_bus_parent(AudioBus bus, AudioBus parent) {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return;
    }
    if (bus == AudioBus::Master) {
        m_parents[index] = AudioBus::Master;
        return;
    }
    m_parents[index] = parent;
}

AudioBus AudioBusMixer::bus_parent(AudioBus bus) const {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return AudioBus::Master;
    }
    return m_parents[index];
}

float AudioBusMixer::routed_bus_gain(AudioBus bus) const {
    if (bus == AudioBus::Master) {
        return 1.f;
    }

    float gain = bus_gain(bus);
    AudioBus current = bus_parent(bus);
    while (current != AudioBus::Master) {
        gain *= bus_gain(current);
        current = bus_parent(current);
    }
    return gain;
}

float AudioBusMixer::effective_gain(AudioBus bus) const {
    if (bus == AudioBus::Master) {
        return bus_gain(AudioBus::Master);
    }
    return routed_bus_gain(bus) * bus_gain(AudioBus::Master);
}

float AudioBusMixer::effective_output_gain(AudioBus bus, float listener_master_volume) const {
    return clamp_bus_gain(listener_master_volume) * effective_gain(bus);
}

} // namespace fuse::audio
