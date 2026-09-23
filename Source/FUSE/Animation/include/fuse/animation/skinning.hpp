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

/// True when this build contains a CUDA skinning kernel (none yet; skinning runs on the CPU).
bool skinning_cuda_kernel_available();

/// Backend skinning actually runs on: Cuda only with a CUDA skinning kernel *and* a usable CUDA
/// device (`fuse::jobs::cudaJobsAvailable()`); CpuReference otherwise — compiling with the CUDA
/// toolkit alone does not change it.
SkinningBackend skinning_backend();

/// CPU reference linear blend skinning. Normals use the bone matrices' linear part and are
/// renormalized. Returns false when there are no vertices or the weight/normal arrays do not
/// match the vertex count (normals may also be empty, in which case none are produced).
bool skin_vertices(const SkinningInput& input, SkinningOutput& output);

/// Skinning palette (bone buffer contents): out[i] = pose world[i] * skeleton inverse_bind[i].
/// Reuses `out`'s storage, so steady-state per-frame calls do not allocate.
void compute_skinning_palette(const Skeleton& skel, const Pose& pose, std::vector<mat4>& out);

} // namespace fuse::animation
