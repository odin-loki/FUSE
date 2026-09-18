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

/// True when \p zones is null or \p zone_count is zero (empty reverb zone list guard).
bool is_empty_reverb_zone_list(const ReverbZoneParams* zones, u32 zone_count);

/// Count zones whose AABB contains the listener.
u32 count_listener_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Clamp a wet-mix scalar into [0, 1].
float clamp_wet_mix(float wet_mix);

/// Complement of a clamped wet mix — dry contribution weight.
float compute_dry_mix(float wet_mix);

/// True when the blend carries at least one active zone.
bool has_active_reverb_blend(const ReverbZoneBlend& blend);

/// True when no active zones contribute wet signal (fully dry path).
bool is_dry_reverb_blend(const ReverbZoneBlend& blend);

/// True when wet convolution should run for the blend result.
bool should_apply_reverb_wet_mix(const ReverbZoneBlend& blend);

/// True when a clamped wet mix is at or below the near-zero epsilon.
bool is_near_zero_wet_mix(float wet_mix);

/// True when wet convolution should be skipped for the blend result.
bool should_skip_reverb_wet_mix(const ReverbZoneBlend& blend);

/// One-shot wet-mix skip from listener position and zone list (empty list → skip).
bool should_skip_reverb_wet_mix(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Effective wet mix scalar [0, 1] from a zone blend result.
float compute_effective_wet_mix(const ReverbZoneBlend& blend);

/// One-shot effective wet mix from listener position and zone list (empty list → 0).
float compute_effective_wet_mix(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Linear dry/wet sample blend stub — wet_mix in [0, 1].
float blend_dry_wet_sample(float dry, float wet, float wet_mix);

/// Dry/wet sample blend from a zone blend result — skips wet path when blend is dry.
float blend_dry_wet_from_reverb_blend(const ReverbZoneBlend& blend, float dry, float wet);

} // namespace fuse::audio
