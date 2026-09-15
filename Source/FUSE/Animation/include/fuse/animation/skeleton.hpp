#pragma once

#include <fuse/animation/math.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace fuse::animation {

struct Bone {
    char name[64] = {};
    s32 parent_index = -1;
    mat4 inverse_bind = mat4::identity();
    mat4 local_transform = mat4::identity();
};

struct Skeleton {
    std::vector<Bone> bones;
    u32 bone_count = 0;

    s32 find_bone(const char* name) const;
    mat4 compute_world_transform(u32 bone_idx) const;
};

struct Pose {
    std::vector<mat4> bone_world_transforms;
    u32 bone_count = 0;

    static Pose make_bind_pose(const Skeleton& skel);
};

} // namespace fuse::animation
