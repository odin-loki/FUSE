#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

/// Normalize a pole hint; when zero or parallel to `root_to_target`, pick a stable bend axis.
[[nodiscard]] vec3 normalize_ik_pole_vector(const vec3& pole, const vec3& root_to_target);

/// Segment lengths from three world-space joint positions. Returns false when either length is near zero.
[[nodiscard]] bool two_bone_segment_lengths(const vec3& root,
                                             const vec3& mid,
                                             const vec3& end,
                                             f32& out_upper_len,
                                             f32& out_lower_len);

/// True when `target` lies within the reachable sphere (upper + lower − `reach_epsilon`).
[[nodiscard]] bool is_two_bone_target_reachable(const vec3& root,
                                                 const vec3& target,
                                                 f32 upper_len,
                                                 f32 lower_len,
                                                 f32 reach_epsilon);

/// World-space distance from root to target (O(1)).
[[nodiscard]] f32 two_bone_root_to_target_distance(const vec3& root, const vec3& target);

/// True when `target` lies outside the reachable sphere or within `reach_epsilon` of the root.
[[nodiscard]] bool needs_two_bone_target_clamp(const vec3& root,
                                                const vec3& target,
                                                f32 upper_len,
                                                f32 lower_len,
                                                f32 reach_epsilon);

/// Maximum root→target distance before clamping (upper + lower − `reach_epsilon`).
[[nodiscard]] f32 two_bone_max_reach(f32 upper_len, f32 lower_len, f32 reach_epsilon);

/// Returns false when the skeleton is empty or indices do not form a root→mid→end chain.
[[nodiscard]] bool is_valid_two_bone_chain(u32 root_bone,
                                            u32 mid_bone,
                                            u32 end_bone,
                                            const Skeleton& skel);

/// Returns false when `bone_indices` has fewer than two entries, any index is out of range, indices repeat,
/// or consecutive bones are not parent→child in the skeleton hierarchy.
[[nodiscard]] bool is_contiguous_bone_chain(const std::vector<u32>& bone_indices, const Skeleton& skel);

/// True when `pose` has no bones, mismatched bone count, or empty world transforms for the skeleton.
[[nodiscard]] bool needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel);

/// Seed `pose` from skeleton bind pose when it is empty or mismatched.
void ensure_pose_bind_fallback(Pose& pose, const Skeleton& skel);

/// Clamp `target` to the reachable sphere defined by segment lengths and `reach_epsilon`.
[[nodiscard]] vec3 clamp_two_bone_target(const vec3& root,
                                          const vec3& target,
                                          f32 upper_len,
                                          f32 lower_len,
                                          f32 reach_epsilon);

/// Closed-form two-bone position solve (O(1)). Returns false when either segment length is zero.
[[nodiscard]] bool solve_two_bone_positions(const vec3& root,
                                             const vec3& mid_bind,
                                             const vec3& end_bind,
                                             const vec3& target,
                                             const vec3& pole_vector,
                                             f32 reach_epsilon,
                                             vec3& out_mid,
                                             vec3& out_end);

struct FABRIKChain {
    std::vector<u32> bone_indices;
    vec3 target = {};
    u32 max_iterations = 10;
    f32 tolerance = 0.001f;
    f32 min_angle_deg = 0.f;
    f32 max_angle_deg = 160.f;

    /// Returns false when the skeleton is empty, the index list has fewer than two bones, any index is out of range,
    /// indices repeat, or consecutive bones are not parent→child in the skeleton hierarchy.
    [[nodiscard]] bool has_valid_chain(const Skeleton& skel) const;

    /// Returns false when the pose is empty or any chain bone index is out of range for the pose buffer.
    [[nodiscard]] bool has_valid_pose(const Pose& pose) const;

    /// True when `pose` needs bind fallback before solving against `skel`.
    [[nodiscard]] bool needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel) const;

    /// Returns false when `has_valid_chain` is false.
    [[nodiscard]] bool solve(Pose& pose, const Skeleton& skel);
};

struct TwoBoneIK {
    u32 root_bone = 0;
    u32 mid_bone = 0;
    u32 end_bone = 0;
    vec3 target = {};
    vec3 pole_vector = {0.f, 1.f, 0.f, 0.f};
    f32 reach_epsilon = 1e-4f;

    /// Returns false when the skeleton is empty, indices are out of range, duplicated, or not a root→mid→end chain.
    [[nodiscard]] bool has_valid_chain(const Skeleton& skel) const;

    /// Returns false when the pose is empty or any chain bone index is out of range for the pose buffer.
    [[nodiscard]] bool has_valid_pose(const Pose& pose) const;

    /// SoA variant of `has_valid_pose`.
    [[nodiscard]] bool has_valid_pose(const PoseSoA& pose) const;

    /// True when the AoS pose needs bind fallback before solving against `skel`.
    [[nodiscard]] bool needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel) const;

    /// SoA variant of `needs_pose_bind_fallback`.
    [[nodiscard]] bool needs_pose_bind_fallback(const PoseSoA& pose, const Skeleton& skel) const;

    /// True when either limb segment has near-zero length in the current pose.
    [[nodiscard]] bool has_degenerate_segments(const Pose& pose) const;

    /// SoA variant of `has_degenerate_segments`.
    [[nodiscard]] bool has_degenerate_segments(const PoseSoA& pose) const;

    /// Upper + lower segment length from the current pose, minus `reach_epsilon` (matches clamp behaviour).
    [[nodiscard]] f32 max_reach(const Pose& pose) const;

    /// Pole vector after zero/parallel fallback relative to the current root→target direction.
    [[nodiscard]] vec3 effective_pole_vector(const Pose& pose) const;

    /// Closed-form two-bone IK (O(1)). Solves in-place on the current pose; returns false when invalid.
    bool solve(Pose& pose, const Skeleton& skel);

    /// SoA variant — writes local positions for the three-bone chain, then recomputes world transforms.
    bool solve(PoseSoA& pose, const Skeleton& skel);
};

} // namespace fuse::animation
