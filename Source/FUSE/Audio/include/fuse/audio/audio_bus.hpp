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

/// Clamp a bus gain multiplier to the valid stub range [0, 1].
float clamp_bus_gain(float gain);

/// True when \p bus is a registered category bus (not \c Count or out of range).
bool is_valid_audio_bus(AudioBus bus);

/// Per-bus gain stub — effective output walks the parent chain to Master.
class AudioBusMixer {
public:
    AudioBusMixer();

    void set_bus_gain(AudioBus bus, float gain);
    float bus_gain(AudioBus bus) const;

    /// Stub parent routing — category buses default to Master.
    void set_bus_parent(AudioBus bus, AudioBus parent);
    AudioBus bus_parent(AudioBus bus) const;

    /// Product of gains along the parent chain from \p bus up to (but excluding) Master.
    float routed_bus_gain(AudioBus bus) const;

    /// `routed_bus_gain(bus) * bus_gain(Master)`.
    float effective_gain(AudioBus bus) const;

    /// Listener master volume × effective bus gain.
    float effective_output_gain(AudioBus bus, float listener_master_volume) const;

    /// Reset all bus gains to unity and restore default parent routing.
    void reset_gains();

    /// True when \p bus is valid and effective gain is above the mute epsilon.
    bool should_apply_bus_gain(AudioBus bus) const;

private:
    float m_gains[static_cast<u32>(AudioBus::Count)] = {1.f, 1.f, 1.f, 1.f};
    AudioBus m_parents[static_cast<u32>(AudioBus::Count)] = {
        AudioBus::Master, AudioBus::Master, AudioBus::Master, AudioBus::Master};
};

/// One-shot effective output gain with empty-bus guard (invalid bus → listener master only).
float compute_effective_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                                  float listener_master_volume);

/// True when \p bus is valid and its effective gain is zero (muted stub).
bool is_bus_muted(const AudioBusMixer& mixer, AudioBus bus);

} // namespace fuse::audio
