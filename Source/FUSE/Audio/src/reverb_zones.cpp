#include <fuse/audio/reverb_zones.hpp>

#include <algorithm>

namespace fuse::audio {

bool has_reverb_zones(const ReverbZoneParams* zones, u32 zone_count) {
    return zones != nullptr && zone_count > 0;
}

bool is_empty_reverb_zones(const ReverbZoneParams* zones, u32 zone_count) {
    return !has_reverb_zones(zones, zone_count);
}

bool should_skip_reverb_zone_blend(const ReverbZoneParams* zones, u32 zone_count) {
    return is_empty_reverb_zones(zones, zone_count);
}

bool listener_in_reverb_zone(const Vec3& listener, const ReverbZoneParams& zone) {
    return zone.bounds.contains(listener);
}

ReverbZoneBlend blend_reverb_zones(const Vec3& listener, const ReverbZoneParams* zones,
                                   u32 zone_count) {
    ReverbZoneBlend blend;
    if (should_skip_reverb_zone_blend(zones, zone_count)) {
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
    if (should_skip_reverb_zone_blend(zones, zone_count)) {
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

float clamp_wet_mix(float wet_mix) {
    return std::clamp(wet_mix, 0.f, 1.f);
}

float compute_dry_mix(float wet_mix) {
    return 1.f - clamp_wet_mix(wet_mix);
}

bool has_active_reverb_blend(const ReverbZoneBlend& blend) {
    return blend.active_zone_count > 0;
}

float compute_effective_wet_mix(const ReverbZoneBlend& blend) {
    if (!has_active_reverb_blend(blend)) {
        return 0.f;
    }
    return clamp_wet_mix(blend.wet_dry) * clamp_wet_mix(blend.send_level);
}

bool is_dry_reverb_blend(const ReverbZoneBlend& blend) {
    return !has_active_reverb_blend(blend) || compute_effective_wet_mix(blend) <= 0.f;
}

bool should_apply_reverb_wet_mix(const ReverbZoneBlend& blend) {
    return has_active_reverb_blend(blend) && compute_effective_wet_mix(blend) > 0.f;
}

bool should_skip_wet_mix_processing(const ReverbZoneBlend& blend) {
    return !should_apply_reverb_wet_mix(blend);
}

float compute_effective_wet_mix(const Vec3& listener, const ReverbZoneParams* zones,
                                u32 zone_count) {
    if (should_skip_reverb_zone_blend(zones, zone_count)) {
        return 0.f;
    }
    return compute_effective_wet_mix(blend_reverb_zones(listener, zones, zone_count));
}

float blend_dry_wet_sample(float dry, float wet, float wet_mix) {
    const float mix = clamp_wet_mix(wet_mix);
    if (mix <= 0.f) {
        return dry;
    }
    if (mix >= 1.f) {
        return wet;
    }
    return dry * compute_dry_mix(mix) + wet * mix;
}

float compute_effective_send_gain(const ReverbZoneBlend& blend) {
    if (!has_active_reverb_blend(blend)) {
        return 0.f;
    }
    return clamp_wet_mix(blend.send_level);
}

float blend_reverb_sample(float dry, float wet, const Vec3& listener, const ReverbZoneParams* zones,
                          u32 zone_count) {
    if (should_skip_reverb_zone_blend(zones, zone_count)) {
        return dry;
    }
    const ReverbZoneBlend blend = blend_reverb_zones(listener, zones, zone_count);
    if (should_skip_wet_mix_processing(blend)) {
        return dry;
    }
    return blend_dry_wet_sample(dry, wet, compute_effective_wet_mix(blend));
}

} // namespace fuse::audio
