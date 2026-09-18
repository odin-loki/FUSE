#include <fuse/audio/occlusion.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

float clamp_occlusion_visibility(float visibility) {
    return std::clamp(visibility, 0.f, 1.f);
}

bool is_fully_visible_occlusion(float visibility) {
    return clamp_occlusion_visibility(visibility) >= 1.f;
}

bool is_fully_occluded_occlusion(float visibility) {
    return clamp_occlusion_visibility(visibility) <= 0.f;
}

bool has_occlusion_blockers(const AABB* blockers, u32 blocker_count) {
    return blockers != nullptr && blocker_count > 0;
}

float clamp_blocker_factor(float factor) {
    return std::clamp(factor, 0.f, 1.f);
}

bool is_clear_blocker_factor(float factor) {
    return clamp_blocker_factor(factor) <= 0.f;
}

bool is_fully_blocked_blocker_factor(float factor) {
    return clamp_blocker_factor(factor) >= 1.f;
}

bool should_skip_blocker_evaluation(const Vec3& listener, const Vec3& source) {
    const Vec3 delta = source - listener;
    return delta.dot(delta) < 1e-12f;
}

bool should_evaluate_occlusion_blockers(const AABB* blockers, u32 blocker_count,
                                        const Vec3& listener, const Vec3& source,
                                        float source_occlusion) {
    if (!has_occlusion_blockers(blockers, blocker_count)) {
        return false;
    }
    if (is_fully_occluded_occlusion(source_occlusion)) {
        return false;
    }
    if (should_skip_blocker_evaluation(listener, source)) {
        return false;
    }
    return true;
}

bool should_skip_occlusion_blocker_evaluation(const AABB* blockers, u32 blocker_count,
                                              const Vec3& listener, const Vec3& source,
                                              float source_occlusion) {
    return !should_evaluate_occlusion_blockers(blockers, blocker_count, listener, source,
                                               source_occlusion);
}

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

float evaluate_occlusion_gain(float visibility, const OcclusionParams& params) {
    if (is_fully_visible_occlusion(visibility)) {
        return 1.f;
    }
    if (is_fully_occluded_occlusion(visibility)) {
        return std::clamp(params.min_gain, 0.f, 1.f);
    }
    const float clamped = clamp_occlusion_visibility(visibility);
    return params.min_gain + (1.f - params.min_gain) * clamped;
}

float evaluate_occlusion_hf_gain(float visibility, const OcclusionParams& params) {
    if (is_fully_visible_occlusion(visibility)) {
        return 1.f;
    }
    if (is_fully_occluded_occlusion(visibility)) {
        return std::clamp(params.hf_attenuation, 0.f, 1.f);
    }
    const float clamped = clamp_occlusion_visibility(visibility);
    const float hf_floor = std::clamp(params.hf_attenuation, 0.f, 1.f);
    return hf_floor + (1.f - hf_floor) * clamped;
}

OcclusionAttenuation evaluate_occlusion_attenuation(float visibility, const OcclusionParams& params) {
    if (is_fully_visible_occlusion(visibility)) {
        return {1.f, 1.f};
    }
    if (is_fully_occluded_occlusion(visibility)) {
        return {std::clamp(params.min_gain, 0.f, 1.f),
                std::clamp(params.hf_attenuation, 0.f, 1.f)};
    }
    return {evaluate_occlusion_gain(visibility, params),
            evaluate_occlusion_hf_gain(visibility, params)};
}

float compute_blocker_visibility(const Vec3& listener, const Vec3& source, const AABB& blocker,
                                 const OcclusionParams& params) {
    if (!segment_intersects_aabb(listener, source, blocker)) {
        return 1.f;
    }
    return std::clamp(params.blocked_visibility, 0.f, 1.f);
}

float compute_blockers_visibility(const Vec3& listener, const Vec3& source, const AABB* blockers,
                                  u32 blocker_count, const OcclusionParams& params) {
    if (should_skip_occlusion_blocker_evaluation(blockers, blocker_count, listener, source, 1.f)) {
        return 1.f;
    }
    float visibility = 1.f;
    for (u32 i = 0; i < blocker_count; ++i) {
        visibility = std::min(visibility,
                               compute_blocker_visibility(listener, source, blockers[i], params));
    }
    return visibility;
}

float compute_blocker_factor(const Vec3& listener, const Vec3& source, const AABB& blocker,
                             const OcclusionParams& params) {
    return 1.f - compute_blocker_visibility(listener, source, blocker, params);
}

float compute_blockers_factor(const Vec3& listener, const Vec3& source, const AABB* blockers,
                              u32 blocker_count, const OcclusionParams& params) {
    if (should_skip_occlusion_blocker_evaluation(blockers, blocker_count, listener, source, 1.f)) {
        return 0.f;
    }
    float factor = 0.f;
    for (u32 i = 0; i < blocker_count; ++i) {
        factor = std::max(factor, compute_blocker_factor(listener, source, blockers[i], params));
    }
    return factor;
}

float combine_occlusion_visibility(float source_occlusion, float blocker_factor) {
    const float visibility = clamp_occlusion_visibility(source_occlusion);
    const float blocked = clamp_blocker_factor(blocker_factor);
    if (is_clear_blocker_factor(blocked)) {
        return visibility;
    }
    if (is_fully_blocked_blocker_factor(blocked)) {
        return 0.f;
    }
    if (is_fully_occluded_occlusion(visibility)) {
        return 0.f;
    }
    return visibility * (1.f - blocked);
}

float compute_effective_visibility(const Vec3& listener, const Vec3& source,
                                   float source_occlusion, const AABB* blockers, u32 blocker_count,
                                   const OcclusionParams& params) {
    const float visibility = clamp_occlusion_visibility(source_occlusion);
    if (!should_evaluate_occlusion_blockers(blockers, blocker_count, listener, source,
                                            source_occlusion)) {
        if (has_occlusion_blockers(blockers, blocker_count)
            && is_fully_occluded_occlusion(visibility)) {
            return 0.f;
        }
        return visibility;
    }
    const float blocker_factor =
        compute_blockers_factor(listener, source, blockers, blocker_count, params);
    return combine_occlusion_visibility(visibility, blocker_factor);
}

OcclusionAttenuation evaluate_occlusion_from_blockers(const Vec3& listener, const Vec3& source,
                                                      float source_occlusion, const AABB* blockers,
                                                      u32 blocker_count,
                                                      const OcclusionParams& params) {
    const float visibility =
        compute_effective_visibility(listener, source, source_occlusion, blockers, blocker_count,
                                     params);
    if (is_fully_visible_occlusion(visibility)) {
        return {1.f, 1.f};
    }
    if (is_fully_occluded_occlusion(visibility)) {
        return {std::clamp(params.min_gain, 0.f, 1.f),
                std::clamp(params.hf_attenuation, 0.f, 1.f)};
    }
    return evaluate_occlusion_attenuation(visibility, params);
}

} // namespace fuse::audio
