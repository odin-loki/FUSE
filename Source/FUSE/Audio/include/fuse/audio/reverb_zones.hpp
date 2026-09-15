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

/// Count zones whose AABB contains the listener.
u32 count_listener_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Effective wet mix scalar [0, 1] from a zone blend result.
float compute_effective_wet_mix(const ReverbZoneBlend& blend);

/// One-shot effective wet mix from listener position and zone list (empty list → 0).
float compute_effective_wet_mix(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count);

/// Linear dry/wet sample blend stub — wet_mix in [0, 1].
float blend_dry_wet_sample(float dry, float wet, float wet_mix);

} // namespace fuse::audio
