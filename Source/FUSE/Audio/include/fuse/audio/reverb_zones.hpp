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

/// True when the zone list is null or empty — reverb blend early-outs to dry.
bool should_skip_reverb_zone_blend(const ReverbZoneParams* zones, u32 zone_count);

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

/// True when a clamped wet mix is at or below zero (fully dry path).
bool is_zero_wet_mix(float wet_mix);

/// True when a clamped wet mix is at or above unity (fully wet path).
bool is_full_wet_mix(float wet_mix);

/// True when dry/wet sample blending can early-out (fully dry or fully wet).
bool should_skip_dry_wet_blend(float wet_mix);

/// Complement of a clamped wet mix — dry contribution weight.
float compute_dry_mix(float wet_mix);

/// True when the blend carries at least one active zone.
bool has_active_reverb_blend(const ReverbZoneBlend& blend);

/// True when no active zones contribute wet signal (fully dry path).
bool is_dry_reverb_blend(const ReverbZoneBlend& blend);

/// True when wet convolution should run for the blend result.
bool should_apply_reverb_wet_mix(const ReverbZoneBlend& blend);

/// True when wet convolution should be skipped for the blend result.
bool should_skip_reverb_wet_convolution(const ReverbZoneBlend& blend);

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

/// Dry/wet sample blend from a precomputed zone blend (dry when inactive or zero wet mix).
float blend_reverb_from_blend(float dry, float wet, const ReverbZoneBlend& blend);

} // namespace fuse::audio
