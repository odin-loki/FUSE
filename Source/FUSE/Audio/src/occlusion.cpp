#include <fuse/audio/occlusion.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

namespace {

bool segment_intersects_aabb(const Vec3& start, const Vec3& end, const AABB& box) {
    const Vec3 dir = end - start;
    float t_min = 0.f;
    float t_max = 1.f;

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = axis == 0 ? start.x : (axis == 1 ? start.y : start.z);
        const float delta = axis == 0 ? dir.x : (axis == 1 ? dir.y : dir.z);
        const float box_min = axis == 0 ? box.min.x : (axis == 1 ? box.min.y : box.min.z);
        const float box_max = axis == 0 ? box.max.x : (axis == 1 ? box.max.y : box.max.z);

        if (std::fabs(delta) < 1e-8f) {
            if (origin < box_min || origin > box_max) {
                return false;
            }
            continue;
        }

        const float inv = 1.f / delta;
        float t0 = (box_min - origin) * inv;
        float t1 = (box_max - origin) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max) {
            return false;
        }
    }

    return t_min <= t_max;
}

} // namespace

float evaluate_occlusion_gain(float visibility, const OcclusionParams& params) {
    const float clamped = std::clamp(visibility, 0.f, 1.f);
    return params.min_gain + (1.f - params.min_gain) * clamped;
}

float compute_blocker_visibility(const Vec3& listener, const Vec3& source, const AABB& blocker) {
    if (!segment_intersects_aabb(listener, source, blocker)) {
        return 1.f;
    }
    return 0.25f;
}

} // namespace fuse::audio
