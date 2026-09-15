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

/// Clamp listener master volume to the same stub range as bus gains.
float clamp_listener_master_volume(float volume);

/// True when a clamped gain is above the mute epsilon (audible stub).
bool is_audible_bus_gain(float gain);

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

    /// Stub per-bus mute flag (independent of zero gain).
    void set_bus_muted(AudioBus bus, bool muted);
    bool bus_muted(AudioBus bus) const;

    /// Stub solo — when any category bus is soloed, non-solo buses are silenced.
    void set_bus_solo(AudioBus bus, bool solo);
    bool bus_soloed(AudioBus bus) const;
    bool any_bus_soloed() const;

    /// Clear solo flags on all category buses.
    void clear_all_solo();

    /// Clear explicit mute flags on all category buses.
    void clear_all_mute();

    /// Number of category buses currently soloed (excludes Master).
    u32 soloed_category_count() const;

    /// True when \p bus is valid and effective gain is above the mute epsilon.
    bool should_apply_bus_gain(AudioBus bus) const;

    /// Early-out: false for invalid, muted, near-zero gain, or non-solo buses while solo is active.
    bool should_mix_bus(AudioBus bus) const;

private:
    float m_gains[static_cast<u32>(AudioBus::Count)] = {1.f, 1.f, 1.f, 1.f};
    AudioBus m_parents[static_cast<u32>(AudioBus::Count)] = {
        AudioBus::Master, AudioBus::Master, AudioBus::Master, AudioBus::Master};
    bool m_muted[static_cast<u32>(AudioBus::Count)] = {false, false, false, false};
    bool m_solo[static_cast<u32>(AudioBus::Count)] = {false, false, false, false};
};

/// One-shot effective output gain with empty-bus guard (invalid bus → listener master only).
float compute_effective_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                                  float listener_master_volume);

/// Effective output gain with mute/solo/empty-bus early-outs (returns 0 when \c should_mix_bus is false).
float compute_mix_output_gain(const AudioBusMixer& mixer, AudioBus bus,
                              float listener_master_volume);

/// True when \p bus is invalid (\c Count or out of range) — empty-bus mix guard.
bool is_empty_bus_mix(AudioBus bus);

/// True when a mix gain is at or below the mute epsilon after clamping.
bool is_silent_mix_gain(float mix_gain);

/// Early-out predicate — true when \p bus should be skipped in the mix path.
bool should_skip_bus_mix(const AudioBusMixer& mixer, AudioBus bus);

/// True when solo mode is active and \p bus is a non-solo category bus.
bool is_bus_solo_silenced(const AudioBusMixer& mixer, AudioBus bus);

/// Scale \p sample by \p mix_gain; returns 0 when mix gain is silent.
float apply_bus_mix_sample(float sample, float mix_gain);

/// One-shot bus mix sample with mute/solo/empty-bus guards.
float compute_bus_mix_sample(float sample, const AudioBusMixer& mixer, AudioBus bus,
                             float listener_master_volume);

/// True when \p bus is valid and its effective gain is zero (muted stub).
bool is_bus_muted(const AudioBusMixer& mixer, AudioBus bus);

} // namespace fuse::audio
