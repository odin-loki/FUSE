#include <fuse/audio/audio_bus.hpp>

namespace fuse::audio {

void AudioBusMixer::set_bus_gain(AudioBus bus, float gain) {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return;
    }
    m_gains[index] = std::max(0.f, gain);
}

float AudioBusMixer::bus_gain(AudioBus bus) const {
    const u32 index = static_cast<u32>(bus);
    if (index >= static_cast<u32>(AudioBus::Count)) {
        return 1.f;
    }
    return m_gains[index];
}

float AudioBusMixer::effective_gain(AudioBus bus) const {
    if (bus == AudioBus::Master) {
        return bus_gain(AudioBus::Master);
    }
    return bus_gain(AudioBus::Master) * bus_gain(bus);
}

} // namespace fuse::audio
