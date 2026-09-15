#include <fuse/terrain/queries.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

f32 sample_height(const Heightfield& field, f32 world_x, f32 world_z) {
    return field.sample_height(world_x, world_z);
}

HeightfieldRayHit raycast_heightfield(const Heightfield& field, vec3 origin, vec3 direction, f32 max_distance) {
    HeightfieldRayHit result{};
    if (!field.is_initialized() || max_distance <= 0.f) {
        return result;
    }

    const vec3 dir = direction.normalized();
    if (dir.length() <= 0.f) {
        return result;
    }

    const f32 step = field.desc().world_size / static_cast<f32>(std::max(field.resolution(), 1u));
    const u32 max_steps = static_cast<u32>(std::ceil(max_distance / std::max(step, 0.001f)));

    f32 previous_t = 0.f;
    f32 previous_delta = sample_height(field, origin.x, origin.z) - origin.y;

    for (u32 i = 1; i <= max_steps; ++i) {
        const f32 t = std::min(static_cast<f32>(i) * step, max_distance);
        const vec3 pos = origin + dir * t;
        const f32 surface_delta = sample_height(field, pos.x, pos.z) - pos.y;

        // Cross from above the surface (negative delta) to at/below it (non-negative delta).
        if (previous_delta < 0.f && surface_delta >= 0.f) {
            const f32 denom = surface_delta - previous_delta;
            const f32 alpha = denom > 1e-6f ? -previous_delta / denom : 0.f;
            const f32 hit_t = previous_t + (t - previous_t) * alpha;
            const vec3 hit_pos = origin + dir * hit_t;

            result.hit = true;
            result.distance = hit_t;
            result.position = hit_pos;
            result.normal = field.sample_normal(hit_pos.x, hit_pos.z);
            return result;
        }

        previous_t = t;
        previous_delta = surface_delta;
    }

    return result;
}

} // namespace fuse::terrain
