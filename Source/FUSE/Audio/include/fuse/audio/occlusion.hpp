#pragma once

#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// Occlusion stub parameters — maps visibility to gain and HF rolloff placeholders.
struct OcclusionParams {
    float min_gain = 0.1f;
    float hf_attenuation = 0.6f;
    float blocked_visibility = 0.25f;
};

/// Clamp per-source visibility into the occlusion pipeline domain.
float clamp_occlusion_visibility(float visibility);

/// True when visibility is at or above unity after clamping — skips attenuation mapping.
bool is_fully_visible_occlusion(float visibility);

/// True when visibility is at or below zero after clamping — skips blocker ray evaluation.
bool is_fully_occluded_occlusion(float visibility);

/// True when a blocker list pointer is non-null and carries at least one AABB.
bool has_occlusion_blockers(const AABB* blockers, u32 blocker_count);

/// Clamp a blocker factor scalar into [0, 1].
float clamp_blocker_factor(float factor);

/// True when a clamped blocker factor is at or below zero (clear line-of-sight).
bool is_clear_blocker_factor(float factor);

/// True when a clamped blocker factor is at or above unity (fully blocked segment).
bool is_fully_blocked_blocker_factor(float factor);

/// Co-located listener/source positions skip segment-vs-AABB blocker evaluation.
bool should_skip_blocker_evaluation(const Vec3& listener, const Vec3& source);

/// True when blocker geometry should be evaluated for the listener→source segment.
bool should_evaluate_occlusion_blockers(const AABB* blockers, u32 blocker_count,
                                        const Vec3& listener, const Vec3& source,
                                        float source_occlusion);

/// True when blocker evaluation should be bypassed (empty list, co-located, or fully occluded source).
bool should_skip_occlusion_blocker_evaluation(const AABB* blockers, u32 blocker_count,
                                              const Vec3& listener, const Vec3& source,
                                              float source_occlusion);

/// Map visibility [0, 1] to a gain multiplier. Fully occluded sources retain `min_gain`.
float evaluate_occlusion_gain(float visibility, const OcclusionParams& params = {});

/// HF rolloff stub — fully visible sources keep unity HF; occluded sources lerp toward `hf_attenuation`.
float evaluate_occlusion_hf_gain(float visibility, const OcclusionParams& params = {});

/// Combined LF/HF occlusion attenuation stub for dry-path mixing.
struct OcclusionAttenuation {
    float gain = 1.f;
    float hf_gain = 1.f;
};

OcclusionAttenuation evaluate_occlusion_attenuation(float visibility,
                                                  const OcclusionParams& params = {});

/// Segment-vs-AABB ray stub — true when the listener→source segment intersects the box.
bool segment_intersects_aabb(const Vec3& listener, const Vec3& source, const AABB& blocker);

/// Line-of-sight stub — returns reduced visibility when a blocker AABB intersects the segment.
float compute_blocker_visibility(const Vec3& listener, const Vec3& source, const AABB& blocker,
                               const OcclusionParams& params = {});

/// Combine visibility across multiple blocker AABBs — returns the minimum visibility encountered.
float compute_blockers_visibility(const Vec3& listener, const Vec3& source, const AABB* blockers,
                                  u32 blocker_count, const OcclusionParams& params = {});

/// Blocker occlusion amount in [0, 1] — 0 = clear LOS, 1 = fully blocked by geometry.
float compute_blocker_factor(const Vec3& listener, const Vec3& source, const AABB& blocker,
                             const OcclusionParams& params = {});

/// Maximum blocker factor across multiple AABBs (worst-case occlusion along the segment).
float compute_blockers_factor(const Vec3& listener, const Vec3& source, const AABB* blockers,
                                u32 blocker_count, const OcclusionParams& params = {});

/// Combine per-source occlusion visibility with blocker factor in [0, 1].
float combine_occlusion_visibility(float source_occlusion, float blocker_factor);

/// Effective visibility from listener, source, per-source occlusion, and optional blockers.
float compute_effective_visibility(const Vec3& listener, const Vec3& source,
                                 float source_occlusion, const AABB* blockers, u32 blocker_count,
                                 const OcclusionParams& params = {});

/// Evaluate LF/HF attenuation from blocker geometry and per-source occlusion.
OcclusionAttenuation evaluate_occlusion_from_blockers(const Vec3& listener, const Vec3& source,
                                                      float source_occlusion, const AABB* blockers,
                                                      u32 blocker_count,
                                                      const OcclusionParams& params = {});

} // namespace fuse::audio
