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

} // namespace fuse::audio
