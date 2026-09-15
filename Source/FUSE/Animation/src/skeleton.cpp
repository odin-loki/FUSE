#include <fuse/animation/skeleton.hpp>

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
    Pose pose;
    pose.bone_count = skel.bone_count;
    pose.bone_world_transforms.resize(skel.bones.size(), mat4::identity());

    for (u32 i = 0; i < skel.bones.size(); ++i) {
        pose.bone_world_transforms[i] = skel.compute_world_transform(i);
    }
    return pose;
}

} // namespace fuse::animation
