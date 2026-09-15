#include <fuse/audio/reverb_zones.hpp>

#include <algorithm>

namespace fuse::audio {

bool listener_in_reverb_zone(const Vec3& listener, const ReverbZoneParams& zone) {
    return zone.bounds.contains(listener);
}

ReverbZoneBlend blend_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                   u32 zone_count) {
    ReverbZoneBlend blend;
    if (zones == nullptr || zone_count == 0) {
        return blend;
    }

    float wet_sum = 0.f;
    float send_sum = 0.f;
    for (u32 i = 0; i < zone_count; ++i) {
        if (!listener_in_reverb_zone(listener, zones[i])) {
            continue;
        }
        wet_sum += std::clamp(zones[i].wet_dry, 0.f, 1.f);
        send_sum += std::clamp(zones[i].send_level, 0.f, 1.f);
        ++blend.active_zone_count;
    }

    if (blend.active_zone_count == 0) {
        return blend;
    }

    const float inv = 1.f / static_cast<float>(blend.active_zone_count);
    blend.wet_dry = wet_sum * inv;
    blend.send_level = send_sum * inv;
    return blend;
}

u32 count_listener_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count) {
    if (zones == nullptr || zone_count == 0) {
        return 0;
    }

    u32 count = 0;
    for (u32 i = 0; i < zone_count; ++i) {
        if (listener_in_reverb_zone(listener, zones[i])) {
            ++count;
        }
    }
    return count;
}

float compute_effective_wet_mix(const ReverbZoneBlend& blend) {
    if (blend.active_zone_count == 0) {
        return 0.f;
    }
    return std::clamp(blend.wet_dry, 0.f, 1.f) * std::clamp(blend.send_level, 0.f, 1.f);
}

float blend_dry_wet_sample(float dry, float wet, float wet_mix) {
    const float mix = std::clamp(wet_mix, 0.f, 1.f);
    return dry * (1.f - mix) + wet * mix;
}

} // namespace fuse::audio
