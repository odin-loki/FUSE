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

f32 vec3_dot(const vec3& a, const vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
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

void set_bone_translation(Pose& pose, u32 bone_idx, const vec3& position) {
    if (bone_idx >= pose.bone_world_transforms.size()) {
        return;
    }
    pose.bone_world_transforms[bone_idx].data[12] = position.x;
    pose.bone_world_transforms[bone_idx].data[13] = position.y;
    pose.bone_world_transforms[bone_idx].data[14] = position.z;
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

void write_local_position(PoseSoA& pose, u32 bone_idx, const vec3& world_position, const Skeleton& skel) {
    if (bone_idx >= pose.bone_count) {
        return;
    }

    const s32 parent = skel.bones[bone_idx].parent_index;
    if (parent >= 0 && static_cast<u32>(parent) < pose.bone_count) {
        const vec3 parentWorld = bone_translation_soa(pose, static_cast<u32>(parent));
        pose.local_positions[bone_idx] = vec3_sub(world_position, parentWorld);
    } else {
        pose.local_positions[bone_idx] = world_position;
    }
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
    if (skel.bones.empty()) {
        return pose.bone_count != 0 || !pose.bone_world_transforms.empty();
    }

    return pose.bone_count != static_cast<u32>(skel.bones.size()) || pose.bone_world_transforms.empty();
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

bool FABRIKChain::needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel) const {
    return fuse::animation::needs_pose_bind_fallback(pose, skel);
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

bool FABRIKChain::solve(Pose& pose, const Skeleton& skel) {
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_bind_fallback(pose, skel);
    if (!has_valid_pose(pose)) {
        return false;
    }
    const u32 endBone = bone_indices.back();

    for (u32 iteration = 0; iteration < max_iterations; ++iteration) {
        set_bone_translation(pose, endBone, target);
        const f32 error = vec3_distance(bone_translation(pose, endBone), target);
        if (error <= tolerance) {
            break;
        }

        for (auto it = bone_indices.rbegin() + 1; it != bone_indices.rend(); ++it) {
            const vec3 child = bone_translation(pose, *(it - 1));
            vec3 parent = bone_translation(pose, *it);
            const f32 dist = vec3_distance(parent, child);
            if (dist < 1e-6f) {
                continue;
            }
            const f32 t = 0.5f;
            parent.x = child.x + (parent.x - child.x) * t;
            parent.y = child.y + (parent.y - child.y) * t;
            parent.z = child.z + (parent.z - child.z) * t;
            set_bone_translation(pose, *it, parent);
        }
    }

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

bool TwoBoneIK::needs_pose_bind_fallback(const Pose& pose, const Skeleton& skel) const {
    return fuse::animation::needs_pose_bind_fallback(pose, skel);
}

bool TwoBoneIK::needs_pose_bind_fallback(const PoseSoA& pose, const Skeleton& skel) const {
    return needs_pose_soa_bind_fallback(pose, skel);
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

bool TwoBoneIK::solve(Pose& pose, const Skeleton& skel) {
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_bind_fallback(pose, skel);
    if (!has_valid_pose(pose) || has_degenerate_segments(pose)) {
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

    set_bone_translation(pose, mid_bone, solvedMid);
    set_bone_translation(pose, end_bone, solvedEnd);
    return true;
}

bool TwoBoneIK::solve(PoseSoA& pose, const Skeleton& skel) {
    if (!has_valid_chain(skel)) {
        return false;
    }

    ensure_pose_soa_bind_fallback(pose, skel);
    if (!has_valid_pose(pose) || has_degenerate_segments(pose)) {
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

    write_local_position(pose, mid_bone, solvedMid, skel);
    pose.compute_world_transforms(skel);
    write_local_position(pose, end_bone, solvedEnd, skel);
    pose.compute_world_transforms(skel);
    return true;
}

} // namespace fuse::animation
