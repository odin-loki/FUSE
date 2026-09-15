#pragma once

#include <fuse/types.hpp>

#include <algorithm>

namespace fuse::audio {

/// Mix bus identifiers — stub routing for category-level gain control.
enum class AudioBus : u8 {
    Master = 0,
    Sfx = 1,
    Music = 2,
    Voice = 3,
    Count
};

/// Per-bus gain stub — effective output is bus_gain * master_gain.
class AudioBusMixer {
public:
    void set_bus_gain(AudioBus bus, float gain);
    float bus_gain(AudioBus bus) const;
    float effective_gain(AudioBus bus) const;

private:
    float m_gains[static_cast<u32>(AudioBus::Count)] = {1.f, 1.f, 1.f, 1.f};
};

} // namespace fuse::audio
