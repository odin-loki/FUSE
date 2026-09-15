#include <fuse/animation/ik_solver.hpp>

#include <cmath>

namespace fuse::animation {

namespace {

f32 vec3_distance(const vec3& a, const vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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

void TwoBoneIK::solve(Pose& pose, const Skeleton& skel) {
    pose = Pose::make_bind_pose(skel);
    if (root_bone >= pose.bone_world_transforms.size() ||
        mid_bone >= pose.bone_world_transforms.size() ||
        end_bone >= pose.bone_world_transforms.size()) {
        return;
    }

    const vec3 root = bone_translation(pose, root_bone);
    const vec3 mid = bone_translation(pose, mid_bone);
    const vec3 end = bone_translation(pose, end_bone);

    const f32 upperLen = vec3_distance(root, mid);
    const f32 lowerLen = vec3_distance(mid, end);
    const f32 reach = upperLen + lowerLen;
    const f32 distToTarget = vec3_distance(root, target);

    vec3 solvedEnd = target;
    if (distToTarget >= reach) {
        const f32 scale = (reach > 0.f) ? ((reach - 1e-4f) / distToTarget) : 0.f;
        solvedEnd.x = root.x + (target.x - root.x) * scale;
        solvedEnd.y = root.y + (target.y - root.y) * scale;
        solvedEnd.z = root.z + (target.z - root.z) * scale;
    }

    const vec3 midHint = {
        (root.x + solvedEnd.x) * 0.5f + pole_vector.x * 0.25f,
        (root.y + solvedEnd.y) * 0.5f + pole_vector.y * 0.25f,
        (root.z + solvedEnd.z) * 0.5f + pole_vector.z * 0.25f,
        0.f,
    };

    set_bone_translation(pose, mid_bone, midHint);
    set_bone_translation(pose, end_bone, solvedEnd);
}

} // namespace fuse::animation
