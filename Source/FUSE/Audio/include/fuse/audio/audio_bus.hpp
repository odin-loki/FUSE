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

/// True when \p bus is the Count sentinel or out of range (empty-bus mix guard).
bool is_empty_audio_bus(AudioBus bus);

/// True when a clamped gain is at or below the mute epsilon.
bool is_near_zero_bus_gain(float gain);

/// True when listener master volume is at or below the mute epsilon.
bool is_listener_master_muted(float listener_master_volume);

/// True when \p volume clamps to an audible listener master level.
bool is_listener_master_audible(float volume);

/// True when listener master would silence all bus mix output (near-zero stub).
bool should_skip_listener_master_mix(float listener_master_volume);

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
    bool any_bus_muted() const;
    u32 muted_bus_count() const;

    /// Clear all category mute flags.
    void clear_bus_mute();

    /// Clear explicit mute flag for \p bus (no-op for Master).
    void clear_bus_mute(AudioBus bus);

    /// Stub solo — when any category bus is soloed, non-solo buses are silenced.
    void set_bus_solo(AudioBus bus, bool solo);
    bool bus_soloed(AudioBus bus) const;
    bool any_bus_soloed() const;
    u32 soloed_bus_count() const;

    /// Clear all category solo flags (solo mode off).
    void clear_bus_solo();

    /// True when \p bus is valid and effective gain is above the mute epsilon.
    bool should_apply_bus_gain(AudioBus bus) const;

    /// Early-out: false for invalid, muted, near-zero gain, or non-solo buses while solo is active.
    bool should_mix_bus(AudioBus bus) const;

    /// True when solo mode silences \p bus (non-solo category while another bus is soloed).
    bool is_bus_solo_silenced(AudioBus bus) const;

    /// True when any ancestor category bus carries an explicit mute flag.
    bool is_parent_chain_muted(AudioBus bus) const;

    /// Readable alias — true when any ancestor (excluding \p bus) is explicitly muted.
    bool is_any_ancestor_bus_muted(AudioBus bus) const;

    /// True when \p bus or any ancestor in its parent chain is explicitly muted.
    bool is_bus_parent_chain_muted(AudioBus bus) const;

    /// True when any ancestor in the parent chain (excluding \p bus) is soloed.
    bool is_any_ancestor_bus_soloed(AudioBus bus) const;

    /// True when \p bus or any ancestor is soloed (audible under solo mode).
    bool is_bus_solo_audible(AudioBus bus) const;

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

/// True when bus mix should be skipped (invalid, muted, near-zero, or solo-silenced).
bool should_skip_bus_mix(const AudioBusMixer& mixer, AudioBus bus);

/// True when bus mix should be skipped, including listener master near-zero early-out.
bool should_skip_bus_mix(const AudioBusMixer& mixer, AudioBus bus,
                         float listener_master_volume);

/// True when \p bus will not contribute audible mix output (alias of \c should_skip_bus_mix).
bool is_bus_mix_silenced(const AudioBusMixer& mixer, AudioBus bus);

/// True when \p bus or any ancestor in its parent chain is explicitly muted.
bool is_bus_parent_chain_muted(const AudioBusMixer& mixer, AudioBus bus);

/// Product of ancestor gains from \p bus parent up to (but excluding) Master.
float parent_chain_gain(const AudioBusMixer& mixer, AudioBus bus);

/// True when ancestor gains are audible and no parent is explicitly muted.
bool is_parent_chain_audible(const AudioBusMixer& mixer, AudioBus bus);

/// True when a muted or near-zero parent chain blocks mixing \p bus.
bool should_skip_parent_chain_mix(const AudioBusMixer& mixer, AudioBus bus);

/// True when \p bus is valid and its effective gain is zero (muted stub).
bool is_bus_muted(const AudioBusMixer& mixer, AudioBus bus);

} // namespace fuse::audio
