#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

enum class SkinningBackend {
    CpuReference,
    Cuda,
};

struct SkinningWeights {
    u32 bone_indices[4] = {};
    f32 bone_weights[4] = {1.f, 0.f, 0.f, 0.f};
};

struct SkinningInput {
    std::vector<vec3> rest_positions;
    std::vector<vec3> rest_normals;
    std::vector<SkinningWeights> weights;
    std::vector<mat4> bone_transforms;
};

struct SkinningOutput {
    std::vector<vec3> positions;
    std::vector<vec3> normals;
};

SkinningBackend skinning_backend();
bool skin_vertices(const SkinningInput& input, SkinningOutput& output);

} // namespace fuse::animation
