#include <fuse/animation/retarget.hpp>

namespace fuse::animation {

bool RetargetMap::is_valid() const {
    if (source_bone_count == 0 || target_bone_count == 0) {
        return false;
    }

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone >= source_bone_count || entry.target_bone >= target_bone_count) {
            return false;
        }
    }
    return !bone_map.empty();
}

s32 RetargetMap::find_source_bone(u32 target_bone) const {
    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.target_bone == target_bone) {
            return static_cast<s32>(entry.source_bone);
        }
    }
    return -1;
}

s32 RetargetMap::find_target_bone(u32 source_bone) const {
    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone == source_bone) {
            return static_cast<s32>(entry.target_bone);
        }
    }
    return -1;
}

RetargetMap RetargetMap::build_identity(const Skeleton& skel) {
    RetargetMap map{};
    map.source_bone_count = static_cast<u32>(skel.bones.size());
    map.target_bone_count = map.source_bone_count;

    for (u32 boneIdx = 0; boneIdx < skel.bones.size(); ++boneIdx) {
        RetargetBoneEntry entry{};
        entry.source_bone = boneIdx;
        entry.target_bone = boneIdx;
        entry.translation_scale = 1.f;
        map.bone_map.push_back(entry);
    }

    return map;
}

RetargetMap RetargetMap::build_by_name(const Skeleton& source, const Skeleton& target) {
    RetargetMap map{};
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
        map.bone_map.push_back(entry);
    }

    return map;
}

void RetargetMap::apply_pose_soa(const PoseSoA& source_pose,
                                 const Skeleton& target_skel,
                                 PoseSoA& out_pose) const {
    if (target_skel.bones.empty() || bone_map.empty()) {
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
        out_pose.local_rotations[entry.target_bone] = source_pose.local_rotations[entry.source_bone];
        out_pose.local_scales[entry.target_bone] = source_pose.local_scales[entry.source_bone];
    }

    out_pose.compute_world_transforms(target_skel);
}

void RetargetMap::apply_pose(const Pose& source_pose,
                             const Skeleton& target_skel,
                             Pose& out_pose) const {
    if (target_skel.bones.empty() || bone_map.empty()) {
        out_pose.bone_count = 0;
        out_pose.bone_world_transforms.clear();
        return;
    }

    out_pose = Pose::make_bind_pose(target_skel);

    for (const RetargetBoneEntry& entry : bone_map) {
        if (entry.source_bone >= source_pose.bone_count || entry.target_bone >= out_pose.bone_count) {
            continue;
        }

        out_pose.bone_world_transforms[entry.target_bone] = source_pose.bone_world_transforms[entry.source_bone];
    }
}

} // namespace fuse::animation
