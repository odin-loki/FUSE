#include <fuse/animation/retarget.hpp>

#include <cmath>

namespace fuse::animation {
namespace {

bool rotation_offset_is_finite(const quat& rotation) {
    return std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z) &&
           std::isfinite(rotation.w);
}

bool is_identity_rotation(const quat& rotation) {
    return rotation.x == 0.f && rotation.y == 0.f && rotation.z == 0.f &&
           (rotation.w == 1.f || rotation.w == -1.f);
}

/// Parent-local product `offset * source`. See `RetargetBoneEntry`.
quat compose_parent_local_offset(const quat& offset, const quat& source) {
    const quat normalized = quat_normalize(offset);
    if (is_identity_rotation(normalized)) {
        return source;
    }
    return quat_multiply(normalized, source);
}

/// Left-multiply the copied matrix's linear part by `offset`, keeping translation.
/// `M(offset) * (R * S) = (offset * R) * S`, the same parent-local product as the SoA path.
mat4 compose_world_rotation_offset(const mat4& source_world, const quat& offset) {
    const quat normalized = quat_normalize(offset);
    if (is_identity_rotation(normalized)) {
        return source_world;
    }

    const mat4 rotation = mat4_from_trs(vec3{}, normalized, {1.f, 1.f, 1.f, 0.f});
    mat4 out = source_world;
    for (u32 col = 0; col < 3u; ++col) {
        const f32 x = source_world.data[col * 4u + 0u];
        const f32 y = source_world.data[col * 4u + 1u];
        const f32 z = source_world.data[col * 4u + 2u];
        out.data[col * 4u + 0u] = rotation.data[0] * x + rotation.data[4] * y + rotation.data[8] * z;
        out.data[col * 4u + 1u] = rotation.data[1] * x + rotation.data[5] * y + rotation.data[9] * z;
        out.data[col * 4u + 2u] = rotation.data[2] * x + rotation.data[6] * y + rotation.data[10] * z;
    }
    return out;
}

} // namespace

bool RetargetMap::is_valid() const {
    if (source_bone_count == 0 || target_bone_count == 0) {
        return false;
    }

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone >= source_bone_count || entry.target_bone >= target_bone_count) {
            return false;
        }
        if (!rotation_offset_is_finite(entry.rotation_offset)) {
            return false;
        }
    }
    return !bone_map.empty();
}

bool RetargetMap::is_source_pose_compatible(const PoseSoA& source_pose) const {
    return source_pose.bone_count > 0 && source_pose.bone_count >= source_bone_count;
}

bool RetargetMap::is_source_pose_compatible(const Pose& source_pose) const {
    return source_pose.bone_count > 0 && source_pose.bone_count >= source_bone_count;
}

bool RetargetMap::is_target_pose_compatible(const PoseSoA& target_pose) const {
    return is_valid() && target_pose.bone_count > 0 && target_pose.bone_count >= target_bone_count;
}

bool RetargetMap::is_target_pose_compatible(const Pose& target_pose) const {
    return is_valid() && target_pose.bone_count > 0 && target_pose.bone_count >= target_bone_count;
}

bool RetargetMap::is_source_skeleton_compatible(const Skeleton& source_skel) const {
    return is_valid() && source_skel.bone_count > 0 && source_skel.bone_count == source_bone_count;
}

bool RetargetMap::is_target_skeleton_compatible(const Skeleton& target_skel) const {
    return is_valid() && target_skel.bone_count > 0 && target_skel.bone_count == target_bone_count;
}

bool RetargetMap::are_skeletons_compatible(const Skeleton& source_skel, const Skeleton& target_skel) const {
    return is_source_skeleton_compatible(source_skel) && is_target_skeleton_compatible(target_skel);
}

void RetargetMap::clear() {
    bone_map.clear();
    source_bone_count = 0;
    target_bone_count = 0;
}

bool RetargetMap::add_bone_mapping(u32 source_bone,
                                   u32 target_bone,
                                   f32 translation_scale,
                                   quat rotation_offset) {
    if (source_bone_count > 0 && source_bone >= source_bone_count) {
        return false;
    }
    if (target_bone_count > 0 && target_bone >= target_bone_count) {
        return false;
    }
    if (is_source_mapped(source_bone) || is_target_mapped(target_bone)) {
        return false;
    }
    if (!rotation_offset_is_finite(rotation_offset)) {
        return false;
    }

    RetargetBoneEntry entry{};
    entry.source_bone = source_bone;
    entry.target_bone = target_bone;
    entry.translation_scale = translation_scale;
    entry.rotation_offset = rotation_offset;
    bone_map.push_back(entry);
    return true;
}

bool RetargetMap::is_source_mapped(u32 source_bone) const {
    return find_target_bone(source_bone) >= 0;
}

bool RetargetMap::is_target_mapped(u32 target_bone) const {
    return find_source_bone(target_bone) >= 0;
}

bool RetargetMap::can_apply_pose_soa(const PoseSoA& source_pose, const Skeleton& target_skel) const {
    return is_valid() && is_target_skeleton_compatible(target_skel) && is_source_pose_compatible(source_pose);
}

bool RetargetMap::can_apply_pose(const Pose& source_pose, const Skeleton& target_skel) const {
    return is_valid() && is_target_skeleton_compatible(target_skel) && is_source_pose_compatible(source_pose);
}

s32 RetargetMap::find_source_bone(u32 target_bone) const {
    if (!is_valid()) {
        return -1;
    }

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.target_bone == target_bone) {
            return static_cast<s32>(entry.source_bone);
        }
    }
    return -1;
}

s32 RetargetMap::find_target_bone(u32 source_bone) const {
    if (!is_valid()) {
        return -1;
    }

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone == source_bone) {
            return static_cast<s32>(entry.target_bone);
        }
    }
    return -1;
}

RetargetMap RetargetMap::build_identity(const Skeleton& skel) {
    RetargetMap map{};
    if (skel.bones.empty()) {
        return map;
    }

    map.source_bone_count = static_cast<u32>(skel.bones.size());
    map.target_bone_count = map.source_bone_count;

    for (u32 boneIdx = 0; boneIdx < skel.bones.size(); ++boneIdx) {
        RetargetBoneEntry entry{};
        entry.source_bone = boneIdx;
        entry.target_bone = boneIdx;
        entry.translation_scale = 1.f;
        entry.rotation_offset = {0.f, 0.f, 0.f, 1.f};
        map.bone_map.push_back(entry);
    }

    return map;
}

RetargetMap RetargetMap::build_by_name(const Skeleton& source, const Skeleton& target) {
    RetargetMap map{};
    if (source.bones.empty() || target.bones.empty()) {
        return map;
    }

    map.source_bone_count = static_cast<u32>(source.bones.size());
    map.target_bone_count = static_cast<u32>(target.bones.size());

    for (u32 sourceIdx = 0; sourceIdx < source.bones.size(); ++sourceIdx) {
        const s32 targetIdx = target.find_bone(source.bones[sourceIdx].name);
        if (targetIdx < 0) {
            continue;
        }

        RetargetBoneEntry entry{};
        entry.source_bone = sourceIdx;
        entry.target_bone = static_cast<u32>(targetIdx);
        entry.translation_scale = 1.f;
        entry.rotation_offset = {0.f, 0.f, 0.f, 1.f};
        map.bone_map.push_back(entry);
    }

    return map;
}

void RetargetMap::apply_pose_soa(const PoseSoA& source_pose,
                                 const Skeleton& target_skel,
                                 PoseSoA& out_pose) const {
    if (!can_apply_pose_soa(source_pose, target_skel)) {
        out_pose.clear();
        return;
    }

    out_pose = PoseSoA::from_bind_pose(target_skel);

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone >= source_pose.bone_count || entry.target_bone >= out_pose.bone_count) {
            continue;
        }

        const vec3& srcPos = source_pose.local_positions[entry.source_bone];
        out_pose.local_positions[entry.target_bone] = {
            srcPos.x * entry.translation_scale,
            srcPos.y * entry.translation_scale,
            srcPos.z * entry.translation_scale,
            0.f,
        };
        out_pose.local_rotations[entry.target_bone] =
            compose_parent_local_offset(entry.rotation_offset, source_pose.local_rotations[entry.source_bone]);
        out_pose.local_scales[entry.target_bone] = source_pose.local_scales[entry.source_bone];
    }

    out_pose.compute_world_transforms(target_skel);
}

void RetargetMap::apply_pose(const Pose& source_pose,
                             const Skeleton& target_skel,
                             Pose& out_pose) const {
    if (!can_apply_pose(source_pose, target_skel)) {
        out_pose.bone_count = 0;
        out_pose.bone_world_transforms.clear();
        return;
    }

    out_pose = Pose::make_bind_pose(target_skel);

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone >= source_pose.bone_count || entry.target_bone >= out_pose.bone_count) {
            continue;
        }

        out_pose.bone_world_transforms[entry.target_bone] = compose_world_rotation_offset(
            source_pose.bone_world_transforms[entry.source_bone], entry.rotation_offset);
    }
}

} // namespace fuse::animation
