#include <fuse/animation/skeleton.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::animation {

s32 Skeleton::find_bone(const char* name) const {
    if (name == nullptr) {
        return -1;
    }

    for (u32 i = 0; i < bones.size(); ++i) {
        if (std::strncmp(bones[i].name, name, sizeof(Bone::name)) == 0) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

mat4 Skeleton::compute_world_transform(u32 bone_idx) const {
    if (bone_idx >= bones.size()) {
        return mat4::identity();
    }

    mat4 world = bones[bone_idx].local_transform;
    s32 parent = bones[bone_idx].parent_index;
    while (parent >= 0 && static_cast<u32>(parent) < bones.size()) {
        world = mat4_multiply(bones[static_cast<u32>(parent)].local_transform, world);
        parent = bones[static_cast<u32>(parent)].parent_index;
    }
    return world;
}

Pose Pose::make_bind_pose(const Skeleton& skel) {
    return PoseSoA::from_bind_pose(skel).to_pose();
}

PoseSoA PoseSoA::allocate(u32 bone_capacity) {
    PoseSoA pose{};
    pose.local_positions.reserve(bone_capacity);
    pose.local_rotations.reserve(bone_capacity);
    pose.local_scales.reserve(bone_capacity);
    pose.bone_world_transforms.reserve(bone_capacity);
    return pose;
}

void PoseSoA::resize(u32 count) {
    bone_count = count;
    local_positions.resize(count, {});
    local_rotations.resize(count, {0.f, 0.f, 0.f, 1.f});
    local_scales.resize(count, {1.f, 1.f, 1.f, 0.f});
    bone_world_transforms.resize(count, mat4::identity());
}

void PoseSoA::clear() {
    bone_count = 0;
    local_positions.clear();
    local_rotations.clear();
    local_scales.clear();
    bone_world_transforms.clear();
}

PoseSoA PoseSoA::from_bind_pose(const Skeleton& skel) {
    PoseSoA pose = allocate(static_cast<u32>(skel.bones.size()));
    pose.resize(static_cast<u32>(skel.bones.size()));

    for (u32 i = 0; i < skel.bones.size(); ++i) {
        decompose_trs(skel.bones[i].local_transform,
                      pose.local_positions[i],
                      pose.local_rotations[i],
                      pose.local_scales[i]);
    }

    pose.compute_world_transforms(skel);
    return pose;
}

Pose PoseSoA::to_pose() const {
    Pose pose;
    pose.bone_count = bone_count;
    pose.bone_world_transforms = bone_world_transforms;
    return pose;
}

void PoseSoA::compute_world_transforms(const Skeleton& skel) {
    const u32 count = static_cast<u32>(skel.bones.size());
    if (bone_count != count) {
        resize(count);
    }

    for (u32 i = 0; i < count; ++i) {
        const mat4 local = mat4_from_trs(local_positions[i], local_rotations[i], local_scales[i]);
        const s32 parent = skel.bones[i].parent_index;
        if (parent >= 0 && static_cast<u32>(parent) < count) {
            bone_world_transforms[i] = mat4_multiply(bone_world_transforms[static_cast<u32>(parent)], local);
        } else {
            bone_world_transforms[i] = local;
        }
    }
}

void add_pose_soa(const PoseSoA& base,
                  const PoseSoA& delta,
                  const PoseSoA& bind,
                  f32 weight,
                  const std::vector<u32>& masked_bones,
                  PoseSoA& out) {
    const f32 clamped = std::clamp(weight, 0.f, 1.f);
    out = base;

    for (u32 boneIndex : masked_bones) {
        if (boneIndex >= out.bone_count) {
            continue;
        }

        const vec3 deltaPos = {
            delta.local_positions[boneIndex].x - bind.local_positions[boneIndex].x,
            delta.local_positions[boneIndex].y - bind.local_positions[boneIndex].y,
            delta.local_positions[boneIndex].z - bind.local_positions[boneIndex].z,
            0.f,
        };
        out.local_positions[boneIndex] = {
            base.local_positions[boneIndex].x + clamped * deltaPos.x,
            base.local_positions[boneIndex].y + clamped * deltaPos.y,
            base.local_positions[boneIndex].z + clamped * deltaPos.z,
            0.f,
        };

        const quat bindRot = bind.local_rotations[boneIndex];
        const quat deltaRot = delta.local_rotations[boneIndex];
        const quat additiveRot = lerp(bindRot, deltaRot, 1.f);
        out.local_rotations[boneIndex] = lerp(base.local_rotations[boneIndex], additiveRot, clamped);

        const vec3 deltaScale = {
            delta.local_scales[boneIndex].x - bind.local_scales[boneIndex].x,
            delta.local_scales[boneIndex].y - bind.local_scales[boneIndex].y,
            delta.local_scales[boneIndex].z - bind.local_scales[boneIndex].z,
            0.f,
        };
        out.local_scales[boneIndex] = {
            base.local_scales[boneIndex].x + clamped * deltaScale.x,
            base.local_scales[boneIndex].y + clamped * deltaScale.y,
            base.local_scales[boneIndex].z + clamped * deltaScale.z,
            0.f,
        };
    }
}

void copy_pose_soa_local(const PoseSoA& src, PoseSoA& dst) {
    dst.resize(src.bone_count);
    dst.local_positions = src.local_positions;
    dst.local_rotations = src.local_rotations;
    dst.local_scales = src.local_scales;
}

void accumulate_weighted_pose_soa(PoseSoA& result,
                                  f32& accumulated_weight,
                                  const PoseSoA& entry,
                                  f32 weight) {
    if (weight <= 0.f) {
        return;
    }

    if (accumulated_weight <= 0.f) {
        result = entry;
        accumulated_weight = weight;
        return;
    }

    const f32 alpha = weight / (accumulated_weight + weight);
    PoseSoA blended = PoseSoA::allocate(std::max(result.bone_count, entry.bone_count));
    blend_pose_soa(result, entry, alpha, blended);
    result = blended;
    accumulated_weight += weight;
}

void finalize_weighted_pose_soa(PoseSoA& pose,
                                f32 accumulated_weight,
                                const Skeleton& skel,
                                PoseSoA& out) {
    if (accumulated_weight <= 0.f) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    ensure_pose_soa_bind_fallback(pose, skel);
    pose.compute_world_transforms(skel);
    out = pose;
    ensure_pose_soa_bind_fallback(out, skel);
}

bool pose_soa_matches_bind(const PoseSoA& pose, const Skeleton& skel, f32 epsilon) {
    const PoseSoA bind = PoseSoA::from_bind_pose(skel);
    if (pose.bone_count != bind.bone_count) {
        return false;
    }

    for (u32 i = 0; i < pose.bone_count; ++i) {
        const vec3& pos = pose.local_positions[i];
        const vec3& bindPos = bind.local_positions[i];
        if (std::fabs(pos.x - bindPos.x) > epsilon || std::fabs(pos.y - bindPos.y) > epsilon ||
            std::fabs(pos.z - bindPos.z) > epsilon) {
            return false;
        }

        const quat& rot = pose.local_rotations[i];
        const quat& bindRot = bind.local_rotations[i];
        if (std::fabs(rot.x - bindRot.x) > epsilon || std::fabs(rot.y - bindRot.y) > epsilon ||
            std::fabs(rot.z - bindRot.z) > epsilon || std::fabs(rot.w - bindRot.w) > epsilon) {
            return false;
        }

        const vec3& scale = pose.local_scales[i];
        const vec3& bindScale = bind.local_scales[i];
        if (std::fabs(scale.x - bindScale.x) > epsilon || std::fabs(scale.y - bindScale.y) > epsilon ||
            std::fabs(scale.z - bindScale.z) > epsilon) {
            return false;
        }
    }

    return true;
}

bool pose_soa_columns_valid(const PoseSoA& pose) {
    if (pose.bone_count == 0) {
        return false;
    }

    return pose.local_positions.size() >= pose.bone_count &&
           pose.local_rotations.size() >= pose.bone_count &&
           pose.local_scales.size() >= pose.bone_count &&
           pose.bone_world_transforms.size() >= pose.bone_count;
}

bool needs_pose_soa_bind_fallback(const PoseSoA& pose, const Skeleton& skel) {
    if (pose.bone_count == 0 || pose.bone_count != skel.bone_count) {
        return true;
    }

    return !pose_soa_columns_valid(pose);
}

bool pose_soa_has_valid_layout(const PoseSoA& pose, const Skeleton& skel) {
    if (skel.bone_count == 0 || skel.bones.empty()) {
        return pose.bone_count == 0;
    }

    return pose.bone_count == skel.bone_count && pose_soa_columns_valid(pose);
}

void ensure_pose_soa_bind_fallback(PoseSoA& pose, const Skeleton& skel) {
    if (needs_pose_soa_bind_fallback(pose, skel)) {
        pose = PoseSoA::from_bind_pose(skel);
    }
}

void reset_pose_soa_to_bind(PoseSoA& pose, const Skeleton& skel) {
    if (skel.bones.empty()) {
        pose.clear();
        return;
    }

    pose = PoseSoA::from_bind_pose(skel);
}

void blend_pose_soa(const PoseSoA& a, const PoseSoA& b, f32 weight, PoseSoA& out) {
    const f32 clamped = std::clamp(weight, 0.f, 1.f);
    const u32 count = std::max(a.bone_count, b.bone_count);
    out.resize(count);

    for (u32 i = 0; i < count; ++i) {
        const vec3 posA = (i < a.local_positions.size()) ? a.local_positions[i] : vec3{};
        const vec3 posB = (i < b.local_positions.size()) ? b.local_positions[i] : vec3{};
        const quat rotA = (i < a.local_rotations.size()) ? a.local_rotations[i] : quat{0.f, 0.f, 0.f, 1.f};
        const quat rotB = (i < b.local_rotations.size()) ? b.local_rotations[i] : quat{0.f, 0.f, 0.f, 1.f};
        const vec3 scaleA = (i < a.local_scales.size()) ? a.local_scales[i] : vec3{1.f, 1.f, 1.f, 0.f};
        const vec3 scaleB = (i < b.local_scales.size()) ? b.local_scales[i] : vec3{1.f, 1.f, 1.f, 0.f};

        out.local_positions[i] = lerp(posA, posB, clamped);
        out.local_rotations[i] = lerp(rotA, rotB, clamped);
        out.local_scales[i] = lerp(scaleA, scaleB, clamped);
    }
}

} // namespace fuse::animation
