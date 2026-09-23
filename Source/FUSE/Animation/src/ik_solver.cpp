#include <fuse/animation/ik_solver.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

namespace {

f32 vec3_length(const vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

vec3 vec3_normalize(const vec3& v) {
    const f32 len = vec3_length(v);
    if (len < 1e-8f) {
        return {0.f, 1.f, 0.f, 0.f};
    }
    return {v.x / len, v.y / len, v.z / len, 0.f};
}

vec3 vec3_sub(const vec3& a, const vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, 0.f};
}

vec3 vec3_cross(const vec3& a, const vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        0.f,
    };
}

f32 vec3_distance(const vec3& a, const vec3& b) {
    return vec3_length(vec3_sub(a, b));
}

vec3 pick_fallback_bend_axis(const vec3& dir) {
    vec3 bendAxis = vec3_cross(dir, {0.f, 0.f, 1.f, 0.f});
    if (vec3_length(bendAxis) < 1e-6f) {
        bendAxis = vec3_cross(dir, {0.f, 1.f, 0.f, 0.f});
    }
    return vec3_normalize(bendAxis);
}

vec3 bone_translation(const Pose& pose, u32 bone_idx) {
    if (bone_idx >= pose.bone_world_transforms.size()) {
        return {};
    }
    return mat4_translation(pose.bone_world_transforms[bone_idx]);
}

vec3 bone_translation_soa(const PoseSoA& pose, u32 bone_idx) {
    if (bone_idx >= pose.bone_world_transforms.size()) {
        return {};
    }
    return mat4_translation(pose.bone_world_transforms[bone_idx]);
}

vec3 vec3_add_scaled(const vec3& a, const vec3& dir, f32 scale) {
    return {a.x + dir.x * scale, a.y + dir.y * scale, a.z + dir.z * scale, 0.f};
}

bool is_in_subtree(const Skeleton& skel, u32 bone, u32 subtree_root) {
    s32 current = static_cast<s32>(bone);
    while (current >= 0 && static_cast<u32>(current) < skel.bones.size()) {
        if (static_cast<u32>(current) == subtree_root) {
            return true;
        }
        current = skel.bones[static_cast<u32>(current)].parent_index;
    }
    return false;
}

/// Rigid rotation `rotation` about world-space `pivot`, as an affine matrix.
mat4 rotation_about_pivot(const quat& rotation, const vec3& pivot) {
    const vec3 rotatedPivot = quat_rotate(rotation, pivot);
    return mat4_from_trs(vec3_sub(pivot, rotatedPivot), rotation, {1.f, 1.f, 1.f, 0.f});
}

/// Rotate each chain bone's subtree so the chain passes through `solved` (world matrices, AoS).
void apply_chain_positions(Pose& pose,
                           const Skeleton& skel,
                           const std::vector<u32>& chain,
                           const std::vector<vec3>& solved) {
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        const vec3 pivot = bone_translation(pose, chain[i]);
        const vec3 child = bone_translation(pose, chain[i + 1]);
        const quat delta = quat_from_to(vec3_sub(child, pivot), vec3_sub(solved[i + 1], pivot));
        const mat4 xform = rotation_about_pivot(delta, pivot);
        const u32 count = std::min(pose.bone_count, static_cast<u32>(skel.bones.size()));
        for (u32 bone = 0; bone < count; ++bone) {
            if (is_in_subtree(skel, bone, chain[i])) {
                pose.bone_world_transforms[bone] = mat4_multiply(xform, pose.bone_world_transforms[bone]);
            }
        }
    }
}

/// SoA variant: rewrites the chain bones' local TRS so the chain passes through `solved`.
void apply_chain_positions(PoseSoA& pose,
                           const Skeleton& skel,
                           const std::vector<u32>& chain,
                           const std::vector<vec3>& solved) {
    pose.compute_world_transforms(skel);
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        const u32 bone = chain[i];
        const vec3 pivot = bone_translation_soa(pose, bone);
        const vec3 child = bone_translation_soa(pose, chain[i + 1]);
        const quat delta = quat_from_to(vec3_sub(child, pivot), vec3_sub(solved[i + 1], pivot));
        const mat4 newWorld = mat4_multiply(rotation_about_pivot(delta, pivot), pose.bone_world_transforms[bone]);

        mat4 newLocal = newWorld;
        const s32 parent = skel.bones[bone].parent_index;
        if (parent >= 0 && static_cast<u32>(parent) < pose.bone_count) {
            newLocal = mat4_multiply(mat4_inverse_affine(pose.bone_world_transforms[static_cast<u32>(parent)]),
                                     newWorld);
        }
        decompose_trs(newLocal, pose.local_positions[bone], pose.local_rotations[bone], pose.local_scales[bone]);
        pose.compute_world_transforms(skel);
    }
}

/// Place the chain for a given `blend`: every segment direction d becomes normalize(d + (axis - d) * blend),
/// so 0 keeps the current shape, 1 is straight along `axis`, and negative values bend each segment
/// further away from `axis`. Returns the resulting root->end distance.
f32 reshape_chain(const std::vector<vec3>& source,
                  const std::vector<f32>& lengths,
                  const vec3& axis,
                  f32 blend,
                  std::vector<vec3>& out) {
    out[0] = source[0];
    for (size_t i = 0; i + 1 < source.size(); ++i) {
        const vec3 dir = vec3_normalize(vec3_sub(source[i + 1], source[i]));
        const vec3 mixed = {
            dir.x + (axis.x - dir.x) * blend,
            dir.y + (axis.y - dir.y) * blend,
            dir.z + (axis.z - dir.z) * blend,
            0.f,
        };
        out[i + 1] = vec3_add_scaled(out[i], vec3_normalize(mixed), lengths[i]);
    }
    return vec3_distance(out[0], out.back());
}

/// Warm start: find a blend (bisection) whose reach equals the target distance, then swing the chain
/// rigidly about the root onto the target. The result is a continuous deformation of the input
/// pose (uniformly straightened or curled), so no joint flips sides. Returns false and leaves
/// `points` untouched when no blend in [-kMaxCurl, 1] brackets the target distance (e.g. a
/// perfectly straight chain has no bend to curl).
bool reshape_chain_to_target(std::vector<vec3>& points, const std::vector<f32>& lengths, const vec3& target) {
    constexpr f32 kMaxCurl = 16.f;
    const vec3 root = points[0];
    const f32 wanted = vec3_distance(root, target);
    const f32 current = vec3_distance(root, points.back());
    if (current < 1e-6f) {
        return false;
    }
    const vec3 axis = vec3_normalize(vec3_sub(points.back(), root));
    std::vector<vec3> scratch(points.size());

    // reach(lo) <= wanted <= reach(hi) must hold for the bisection bracket.
    f32 lo = 0.f;
    f32 hi = 0.f;
    if (wanted >= current) {
        hi = 1.f;
    } else {
        lo = -kMaxCurl;
        if (reshape_chain(points, lengths, axis, lo, scratch) > wanted) {
            return false;
        }
    }
    for (u32 step = 0; step < 32; ++step) {
        const f32 mid = 0.5f * (lo + hi);
        if (reshape_chain(points, lengths, axis, mid, scratch) < wanted) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    reshape_chain(points, lengths, axis, 0.5f * (lo + hi), scratch);

    const quat swing = quat_from_to(vec3_sub(scratch.back(), root), vec3_sub(target, root));
    for (size_t i = 0; i < points.size(); ++i) {
        const vec3 local = quat_rotate(swing, vec3_sub(scratch[i], root));
        points[i] = {root.x + local.x, root.y + local.y, root.z + local.z, 0.f};
    }
    return true;
}

/// Core FABRIK on world positions. Root stays pinned; returns passes run and writes the final error.
u32 fabrik_solve_positions(std::vector<vec3>& points,
                           const vec3& target,
                           u32 max_iterations,
                           f32 tolerance,
                           bool reshape_warm_start,
                           f32& out_error) {
    const size_t n = points.size();
    std::vector<f32> lengths(n - 1);
    f32 totalLength = 0.f;
    for (size_t i = 0; i + 1 < n; ++i) {
        lengths[i] = vec3_distance(points[i], points[i + 1]);
        totalLength += lengths[i];
    }

    const vec3 root = points[0];
    const f32 targetDistance = vec3_distance(root, target);
    u32 iterations = 0;
    if (targetDistance >= totalLength) {
        // Unreachable: the closest configuration is the chain fully extended toward the target.
        for (size_t i = 0; i + 1 < n; ++i) {
            const vec3 dir = vec3_normalize(vec3_sub(target, points[i]));
            points[i + 1] = vec3_add_scaled(points[i], dir, lengths[i]);
        }
        iterations = 1;
    } else {
        // FABRIK converges only linearly, slowest near full extension; the reshape warm start is exact
        // whenever it can bracket the target distance, leaving the passes to polish float error or to
        // do the real work when it cannot (e.g. deep folds of a nearly straight chain).
        if (reshape_warm_start && vec3_distance(points[n - 1], target) > tolerance) {
            (void)reshape_chain_to_target(points, lengths, target);
        }
        while (iterations < max_iterations && vec3_distance(points[n - 1], target) > tolerance) {
            // Backward pass: pin the end effector to the target, walk toward the root.
            points[n - 1] = target;
            for (size_t i = n - 1; i-- > 0;) {
                const vec3 dir = vec3_normalize(vec3_sub(points[i], points[i + 1]));
                points[i] = vec3_add_scaled(points[i + 1], dir, lengths[i]);
            }
            // Forward pass: re-pin the root, walk toward the end effector.
            points[0] = root;
            for (size_t i = 0; i + 1 < n; ++i) {
                const vec3 dir = vec3_normalize(vec3_sub(points[i + 1], points[i]));
                points[i + 1] = vec3_add_scaled(points[i], dir, lengths[i]);
            }
            ++iterations;
        }
    }

    out_error = vec3_distance(points[n - 1], target);
    return iterations;
}

} // namespace

vec3 normalize_ik_pole_vector(const vec3& pole, const vec3& root_to_target) {
    const vec3 dir = vec3_normalize(root_to_target);
    if (vec3_length(pole) < 1e-6f) {
        return pick_fallback_bend_axis(dir);
    }

    vec3 bendAxis = vec3_cross(dir, pole);
    if (vec3_length(bendAxis) < 1e-6f) {
        return pick_fallback_bend_axis(dir);
    }
    return vec3_normalize(bendAxis);
}

bool two_bone_segment_lengths(const vec3& root,
                               const vec3& mid,
                               const vec3& end,
                               f32& out_upper_len,
                               f32& out_lower_len) {
    out_upper_len = vec3_distance(root, mid);
    out_lower_len = vec3_distance(mid, end);
    return out_upper_len >= 1e-6f && out_lower_len >= 1e-6f;
}

f32 two_bone_max_reach(f32 upper_len, f32 lower_len, f32 reach_epsilon) {
    if (upper_len < 1e-6f || lower_len < 1e-6f) {
        return 0.f;
    }
    return upper_len + lower_len - reach_epsilon;
}

f32 two_bone_root_to_target_distance(const vec3& root, const vec3& target) {
    return vec3_distance(root, target);
}

bool needs_two_bone_target_clamp(const vec3& root,
                                  const vec3& target,
                                  f32 upper_len,
                                  f32 lower_len,
                                  f32 reach_epsilon) {
    if (upper_len < 1e-6f || lower_len < 1e-6f) {
        return true;
    }

    const f32 dist = two_bone_root_to_target_distance(root, target);
    const f32 maxReach = two_bone_max_reach(upper_len, lower_len, reach_epsilon);
    return dist > maxReach || dist < reach_epsilon;
}

bool is_two_bone_target_reachable(const vec3& root,
                                   const vec3& target,
                                   f32 upper_len,
                                   f32 lower_len,
                                   f32 reach_epsilon) {
    if (upper_len < 1e-6f || lower_len < 1e-6f) {
        return false;
    }

    return !needs_two_bone_target_clamp(root, target, upper_len, lower_len, reach_epsilon);
}

bool is_contiguous_bone_chain(const std::vector<u32>& bone_indices, const Skeleton& skel) {
    if (bone_indices.size() < 2 || skel.bones.empty()) {
        return false;
    }

    const u32 boneCount = static_cast<u32>(skel.bones.size());
    for (size_t i = 0; i < bone_indices.size(); ++i) {
        const u32 boneIdx = bone_indices[i];
        if (boneIdx >= boneCount) {
            return false;
        }

        for (size_t j = 0; j < i; ++j) {
            if (bone_indices[j] == boneIdx) {
                return false;
            }
        }

        if (i > 0) {
            const u32 parentIdx = bone_indices[i - 1];
            if (skel.bones[boneIdx].parent_index != static_cast<s32>(parentIdx)) {
                return false;
            }
        }
    }
    return true;
}

bool needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel) {
    if (pose.bone_count == 0 || pose.bone_count != skel.bone_count) {
        return true;
    }

    return pose.bone_world_transforms.size() < pose.bone_count;
}

void ensure_pose_bind_fallback(Pose& pose, const Skeleton& skel) {
    if (needs_pose_bind_fallback(pose, skel)) {
        pose = Pose::make_bind_pose(skel);
    }
}

bool is_valid_two_bone_chain(u32 root_bone, u32 mid_bone, u32 end_bone, const Skeleton& skel) {
    if (skel.bones.empty()) {
        return false;
    }

    const u32 boneCount = static_cast<u32>(skel.bones.size());
    if (root_bone >= boneCount || mid_bone >= boneCount || end_bone >= boneCount) {
        return false;
    }

    if (root_bone == mid_bone || mid_bone == end_bone || root_bone == end_bone) {
        return false;
    }

    if (skel.bones[mid_bone].parent_index != static_cast<s32>(root_bone)) {
        return false;
    }

    if (skel.bones[end_bone].parent_index != static_cast<s32>(mid_bone)) {
        return false;
    }

    return true;
}

vec3 clamp_two_bone_target(const vec3& root,
                            const vec3& target,
                            f32 upper_len,
                            f32 lower_len,
                            f32 reach_epsilon) {
    vec3 delta = vec3_sub(target, root);
    f32 dist = vec3_length(delta);
    const f32 maxReach = two_bone_max_reach(upper_len, lower_len, reach_epsilon);

    if (dist > maxReach) {
        const vec3 dir = vec3_normalize(delta);
        return {
            root.x + dir.x * maxReach,
            root.y + dir.y * maxReach,
            root.z + dir.z * maxReach,
            0.f,
        };
    }

    if (dist < reach_epsilon) {
        return {
            root.x,
            root.y + reach_epsilon,
            root.z,
            0.f,
        };
    }

    return target;
}

bool solve_two_bone_positions(const vec3& root,
                               const vec3& mid_bind,
                               const vec3& end_bind,
                               const vec3& target,
                               const vec3& pole_vector,
                               f32 reach_epsilon,
                               vec3& out_mid,
                               vec3& out_end) {
    f32 upperLen = 0.f;
    f32 lowerLen = 0.f;
    if (!two_bone_segment_lengths(root, mid_bind, end_bind, upperLen, lowerLen)) {
        return false;
    }

    const vec3 effectiveTarget = clamp_two_bone_target(root, target, upperLen, lowerLen, reach_epsilon);
    vec3 delta = vec3_sub(effectiveTarget, root);
    const f32 dist = vec3_length(delta);
    const vec3 dir = vec3_normalize(delta);

    const f32 cosShoulder =
        (upperLen * upperLen + dist * dist - lowerLen * lowerLen) / (2.f * upperLen * dist);
    const f32 clampedCos = std::clamp(cosShoulder, -1.f, 1.f);
    const f32 shoulderSin = std::sqrt(std::max(0.f, 1.f - clampedCos * clampedCos));

    const vec3 bendAxis = normalize_ik_pole_vector(pole_vector, delta);
    const vec3 secondaryAxis = vec3_normalize(vec3_cross(bendAxis, dir));
    const vec3 upperDir = {
        dir.x * clampedCos + secondaryAxis.x * shoulderSin,
        dir.y * clampedCos + secondaryAxis.y * shoulderSin,
        dir.z * clampedCos + secondaryAxis.z * shoulderSin,
        0.f,
    };

    out_mid = {
        root.x + upperDir.x * upperLen,
        root.y + upperDir.y * upperLen,
        root.z + upperDir.z * upperLen,
        0.f,
    };
    out_end = effectiveTarget;
    return true;
}

bool FABRIKChain::has_valid_chain(const Skeleton& skel) const {
    return is_contiguous_bone_chain(bone_indices, skel);
}

bool FABRIKChain::has_valid_pose(const Pose& pose) const {
    if (bone_indices.empty() || pose.bone_count == 0 || pose.bone_world_transforms.empty()) {
        return false;
    }

    for (u32 boneIdx : bone_indices) {
        if (boneIdx >= pose.bone_count) {
            return false;
        }
    }
    return true;
}

bool FABRIKChain::has_valid_pose(const PoseSoA& pose) const {
    if (bone_indices.empty() || pose.bone_count == 0 || pose.local_positions.empty()) {
        return false;
    }

    for (u32 boneIdx : bone_indices) {
        if (boneIdx >= pose.bone_count) {
            return false;
        }
    }
    return true;
}

bool FABRIKChain::has_degenerate_segments(const Pose& pose) const {
    if (!has_valid_pose(pose) || bone_indices.size() < 2) {
        return true;
    }

    for (size_t i = 1; i < bone_indices.size(); ++i) {
        const vec3 parent = bone_translation(pose, bone_indices[i - 1]);
        const vec3 child = bone_translation(pose, bone_indices[i]);
        if (vec3_distance(parent, child) < 1e-6f) {
            return true;
        }
    }
    return false;
}

bool FABRIKChain::can_solve(const Pose& pose, const Skeleton& skel) const {
    return has_valid_chain(skel) && has_valid_pose(pose) && !has_degenerate_segments(pose);
}

bool FABRIKChain::solve(Pose& pose, const Skeleton& skel) {
    last_iterations = 0;
    last_error = 0.f;
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_bind_fallback(pose, skel);
    if (!can_solve(pose, skel)) {
        return false;
    }

    std::vector<vec3> points(bone_indices.size());
    for (size_t i = 0; i < bone_indices.size(); ++i) {
        points[i] = bone_translation(pose, bone_indices[i]);
    }
    last_iterations = fabrik_solve_positions(points, target, max_iterations, tolerance, reshape_warm_start, last_error);
    apply_chain_positions(pose, skel, bone_indices, points);
    last_error = vec3_distance(bone_translation(pose, bone_indices.back()), target);
    return true;
}

bool FABRIKChain::solve(PoseSoA& pose, const Skeleton& skel) {
    last_iterations = 0;
    last_error = 0.f;
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_soa_bind_fallback(pose, skel);
    if (!has_valid_pose(pose)) {
        return false;
    }
    pose.compute_world_transforms(skel);

    std::vector<vec3> points(bone_indices.size());
    for (size_t i = 0; i < bone_indices.size(); ++i) {
        points[i] = bone_translation_soa(pose, bone_indices[i]);
        if (i > 0 && vec3_distance(points[i - 1], points[i]) < 1e-6f) {
            return false;
        }
    }
    last_iterations = fabrik_solve_positions(points, target, max_iterations, tolerance, reshape_warm_start, last_error);
    apply_chain_positions(pose, skel, bone_indices, points);
    last_error = vec3_distance(bone_translation_soa(pose, bone_indices.back()), target);
    return true;
}

bool TwoBoneIK::has_valid_chain(const Skeleton& skel) const {
    return is_valid_two_bone_chain(root_bone, mid_bone, end_bone, skel);
}

bool TwoBoneIK::has_valid_pose(const Pose& pose) const {
    if (pose.bone_count == 0 || pose.bone_world_transforms.empty()) {
        return false;
    }

    return root_bone < pose.bone_count && mid_bone < pose.bone_count && end_bone < pose.bone_count;
}

bool TwoBoneIK::has_valid_pose(const PoseSoA& pose) const {
    if (pose.bone_count == 0 || pose.local_positions.empty()) {
        return false;
    }

    return root_bone < pose.bone_count && mid_bone < pose.bone_count && end_bone < pose.bone_count;
}

bool TwoBoneIK::has_degenerate_segments(const Pose& pose) const {
    if (!has_valid_pose(pose)) {
        return true;
    }

    const vec3 root = bone_translation(pose, root_bone);
    const vec3 mid = bone_translation(pose, mid_bone);
    const vec3 end = bone_translation(pose, end_bone);
    return vec3_distance(root, mid) < 1e-6f || vec3_distance(mid, end) < 1e-6f;
}

bool TwoBoneIK::has_degenerate_segments(const PoseSoA& pose) const {
    if (!has_valid_pose(pose)) {
        return true;
    }

    const vec3 root = bone_translation_soa(pose, root_bone);
    const vec3 mid = bone_translation_soa(pose, mid_bone);
    const vec3 end = bone_translation_soa(pose, end_bone);
    return vec3_distance(root, mid) < 1e-6f || vec3_distance(mid, end) < 1e-6f;
}

f32 TwoBoneIK::max_reach(const Pose& pose) const {
    if (root_bone >= pose.bone_count || mid_bone >= pose.bone_count || end_bone >= pose.bone_count) {
        return 0.f;
    }

    const vec3 root = bone_translation(pose, root_bone);
    const vec3 mid = bone_translation(pose, mid_bone);
    const vec3 end = bone_translation(pose, end_bone);
    f32 upperLen = 0.f;
    f32 lowerLen = 0.f;
    if (!two_bone_segment_lengths(root, mid, end, upperLen, lowerLen)) {
        return 0.f;
    }
    return two_bone_max_reach(upperLen, lowerLen, reach_epsilon);
}

vec3 TwoBoneIK::effective_pole_vector(const Pose& pose) const {
    if (root_bone >= pose.bone_count) {
        return normalize_ik_pole_vector(pole_vector, {0.f, 1.f, 0.f, 0.f});
    }

    const vec3 root = bone_translation(pose, root_bone);
    const vec3 rootToTarget = vec3_sub(target, root);
    return normalize_ik_pole_vector(pole_vector, rootToTarget);
}

bool TwoBoneIK::can_solve(const Pose& pose, const Skeleton& skel) const {
    return has_valid_chain(skel) && has_valid_pose(pose) && !has_degenerate_segments(pose);
}

bool TwoBoneIK::can_solve(const PoseSoA& pose, const Skeleton& skel) const {
    return has_valid_chain(skel) && has_valid_pose(pose) && !has_degenerate_segments(pose);
}

bool TwoBoneIK::solve(Pose& pose, const Skeleton& skel) {
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_bind_fallback(pose, skel);
    if (!can_solve(pose, skel)) {
        return false;
    }

    const vec3 root = bone_translation(pose, root_bone);
    const vec3 mid = bone_translation(pose, mid_bone);
    const vec3 end = bone_translation(pose, end_bone);

    vec3 solvedMid{};
    vec3 solvedEnd{};
    if (!solve_two_bone_positions(root, mid, end, target, pole_vector, reach_epsilon, solvedMid, solvedEnd)) {
        return false;
    }

    apply_chain_positions(pose, skel, {root_bone, mid_bone, end_bone}, {root, solvedMid, solvedEnd});
    return true;
}

bool TwoBoneIK::solve(PoseSoA& pose, const Skeleton& skel) {
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_soa_bind_fallback(pose, skel);
    if (!can_solve(pose, skel)) {
        return false;
    }

    const vec3 root = bone_translation_soa(pose, root_bone);
    const vec3 mid = bone_translation_soa(pose, mid_bone);
    const vec3 end = bone_translation_soa(pose, end_bone);

    vec3 solvedMid{};
    vec3 solvedEnd{};
    if (!solve_two_bone_positions(root, mid, end, target, pole_vector, reach_epsilon, solvedMid, solvedEnd)) {
        return false;
    }

    apply_chain_positions(pose, skel, {root_bone, mid_bone, end_bone}, {root, solvedMid, solvedEnd});
    return true;
}

} // namespace fuse::animation
