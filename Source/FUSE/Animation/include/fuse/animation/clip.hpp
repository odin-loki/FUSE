#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

struct KeyframeChannel {
    std::vector<f32> times;
    std::vector<vec3> values_vec3;
    std::vector<quat> values_quat;

    vec3 sample_vec3(f32 time) const;
    quat sample_quat(f32 time) const;
};

struct AnimationClip {
    char name[128] = {};
    f32 duration = 0.f;
    f32 sample_rate = 30.f;
    bool looping = true;

    struct BoneChannels {
        u32 bone_index = 0;
        KeyframeChannel position;
        KeyframeChannel rotation;
        KeyframeChannel scale;
    };

    std::vector<BoneChannels> bone_channels;

    void sample(f32 time, const Skeleton& skel, Pose& out_pose) const;
    bool save(const char* path) const;
    bool load(const char* path);
};

} // namespace fuse::animation
