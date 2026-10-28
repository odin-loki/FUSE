#pragma once

#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// Wet/dry mix parameters for a spatial reverb zone (CPU stub — no IR data).
struct ReverbZoneParams {
    AABB bounds;
    float wet_dry = 0.3f;
    float send_level = 1.f;
};

/// True when a zone list pointer is non-null and carries at least one zone.
bool has_reverb_zones(const ReverbZoneParams* zones, u32 zone_count);

/// Readable alias — true when the zone list is null or zero-length.
bool is_empty_reverb_zones(const ReverbZoneParams* zones, u32 zone_count);

/// True when zone blending can be skipped (empty zone list).
bool should_skip_reverb_zone_blend(const ReverbZoneParams* zones, u32 zone_count);

/// True when the listener is inside at least one zone (non-empty list required).
bool listener_has_active_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                    u32 zone_count);

/// True when no zones contribute wet signal for the listener (empty list or outside all zones).
bool should_skip_listener_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                       u32 zone_count);

/// Returns true when the listener position lies inside the zone AABB.
bool listener_in_reverb_zone(const Vec3& listener, const ReverbZoneParams& zone);

/// Blended wet/dry mix from all zones containing the listener.
struct ReverbZoneBlend {
    float wet_dry = 0.f;
    float send_level = 0.f;
    u32 active_zone_count = 0;
};

/// Average wet/dry and send_level across overlapping zones; dry when outside all zones.
ReverbZoneBlend blend_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                   u32 zone_count);

/// Count zones whose AABB contains the listener.
u32 count_listener_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Clamp a wet-mix scalar into [0, 1].
float clamp_wet_mix(float wet_mix);

/// True when a clamped wet-mix scalar is at or below the dry-path epsilon.
bool is_near_zero_wet_mix(float wet_mix);

/// Complement of a clamped wet mix — dry contribution weight.
float compute_dry_mix(float wet_mix);

/// True when the blend carries at least one active zone.
bool has_active_reverb_blend(const ReverbZoneBlend& blend);

/// True when no active zones contribute wet signal (fully dry path).
bool is_dry_reverb_blend(const ReverbZoneBlend& blend);

/// True when wet convolution should run for the blend result.
bool should_apply_reverb_wet_mix(const ReverbZoneBlend& blend);

/// True when wet convolution and sample blending can be skipped (fully dry path).
bool should_skip_wet_mix_processing(const ReverbZoneBlend& blend);

/// True when dry/wet sample blending can be skipped (empty zones, outside listener, or dry blend).
bool should_skip_reverb_sample_blend(const Vec3& listener, const ReverbZoneParams* zones,
                                     u32 zone_count);

/// Effective wet mix scalar [0, 1] from a zone blend result.
float compute_effective_wet_mix(const ReverbZoneBlend& blend);

/// One-shot effective wet mix from listener position and zone list (empty list → 0).
float compute_effective_wet_mix(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Linear dry/wet sample blend stub — wet_mix in [0, 1].
float blend_dry_wet_sample(float dry, float wet, float wet_mix);

/// Effective send gain [0, 1] from a zone blend result (zero when inactive).
float compute_effective_send_gain(const ReverbZoneBlend& blend);

/// One-shot dry/wet sample blend from listener position and zone list (dry when empty/outside).
float blend_reverb_sample(float dry, float wet, const Vec3& listener, const ReverbZoneParams* zones,
                          u32 zone_count);

/// Sends at or below this (after clamping to [0, 1]) add nothing to the listener reverb bus.
inline constexpr float kReverbSendSilence = 1.0e-6f;

/// Clamp a per-source reverb send into [0, 1]. Non-finite values become 0.
float clamp_reverb_send(float send);

/// True when \p send contributes no wet signal (silence, or non-finite).
bool is_silent_reverb_send(float send);

/// Per-source reverb send into the listener bus. Independent of the zone wet mix.
///
///   send_i   = clamp(source.reverb_send, 0, 1)
///   mono_i   = 0.5 * (dry_left_i + dry_right_i)          // spatialized source, pre-sum
///   dry     += dry_i
///   if send_i <= kReverbSendSilence:                     // silence skips the wet contribution
///       (sent and bus are left untouched)
///   else:
///       sent += send_i * dry_i
///       bus  += send_i * mono_i
///   wet      = convolve(bus)                             // listener reverb
///   zone_wet = clamp(zone.wet_dry) * clamp(zone.send_level)
///   out      = (dry - sent) + blend_dry_wet_sample(sent, wet, zone_wet)
///            = (dry - sent) + sent * (1 - zone_wet) + wet * zone_wet
///
/// send_i = 1 → sent = dry → out = blend_dry_wet_sample(dry, wet, zone_wet)
/// (the historical listener crossfade). send_i = 0 leaves that source out of sent and bus.
/// Zone wet mix does not scale send_i; it only crossfades the sent portion.
float contribute_reverb_send(float dry_sample, float source_send);

/// Listener output sample from the dry sum, the sent portion, the convolved bus, and zone wet mix.
float mix_listener_reverb_sample(float dry, float sent, float wet, float zone_wet);

} // namespace fuse::audio
