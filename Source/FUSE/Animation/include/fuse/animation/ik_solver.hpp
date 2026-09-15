#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

struct FABRIKChain {
    std::vector<u32> bone_indices;
    vec3 target = {};
    u32 max_iterations = 10;
    f32 tolerance = 0.001f;
    f32 min_angle_deg = 0.f;
    f32 max_angle_deg = 160.f;

    void solve(Pose& pose, const Skeleton& skel);
};

struct TwoBoneIK {
    u32 root_bone = 0;
    u32 mid_bone = 0;
    u32 end_bone = 0;
    vec3 target = {};
    vec3 pole_vector = {0.f, 1.f, 0.f, 0.f};
    f32 reach_epsilon = 1e-4f;

    /// Closed-form two-bone IK (O(1)). Returns false when bone indices are invalid.
    bool solve(Pose& pose, const Skeleton& skel);

    /// SoA variant — writes local positions for the three-bone chain, then recomputes world transforms.
    bool solve(PoseSoA& pose, const Skeleton& skel);
};

} // namespace fuse::animation
