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

bool solve_two_bone_positions(const vec3& root,
                              const vec3& mid_bind,
                              const vec3& end_bind,
                              const vec3& target,
                              const vec3& pole_vector,
                              f32 reach_epsilon,
                              vec3& out_mid,
                              vec3& out_end) {
    const f32 upperLen = vec3_distance(root, mid_bind);
    const f32 lowerLen = vec3_distance(mid_bind, end_bind);
    if (upperLen < 1e-6f || lowerLen < 1e-6f) {
        return false;
    }

    vec3 delta = vec3_sub(target, root);
    f32 dist = vec3_length(delta);
    const f32 maxReach = upperLen + lowerLen - reach_epsilon;

    vec3 effectiveTarget = target;
    if (dist > maxReach) {
        const vec3 dir = vec3_normalize(delta);
        effectiveTarget = {
            root.x + dir.x * maxReach,
            root.y + dir.y * maxReach,
            root.z + dir.z * maxReach,
            0.f,
        };
        dist = maxReach;
    }
    if (dist < reach_epsilon) {
        dist = reach_epsilon;
        delta = {0.f, reach_epsilon, 0.f, 0.f};
    } else {
        delta = vec3_sub(effectiveTarget, root);
        dist = vec3_length(delta);
    }

    const vec3 dir = vec3_normalize(delta);
    const f32 cosShoulder =
        (upperLen * upperLen + dist * dist - lowerLen * lowerLen) / (2.f * upperLen * dist);
    const f32 clampedCos = std::clamp(cosShoulder, -1.f, 1.f);
    const f32 shoulderSin = std::sqrt(std::max(0.f, 1.f - clampedCos * clampedCos));

    vec3 bendAxis = vec3_cross(dir, pole_vector);
    if (vec3_length(bendAxis) < 1e-6f) {
        bendAxis = vec3_cross(dir, {0.f, 0.f, 1.f, 0.f});
    }
    bendAxis = vec3_normalize(bendAxis);

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

void FABRIKChain::solve(Pose& pose, const Skeleton& skel) {
    if (bone_indices.empty()) {
        return;
    }

    pose = Pose::make_bind_pose(skel);
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
}

bool TwoBoneIK::solve(Pose& pose, const Skeleton& skel) {
    pose = Pose::make_bind_pose(skel);
    if (root_bone >= pose.bone_world_transforms.size() ||
        mid_bone >= pose.bone_world_transforms.size() ||
        end_bone >= pose.bone_world_transforms.size()) {
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
    pose = PoseSoA::from_bind_pose(skel);
    if (root_bone >= pose.bone_count || mid_bone >= pose.bone_count || end_bone >= pose.bone_count) {
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
