#include <fuse/animation/skinning.hpp>

#include <fuse/animation/skinning_kernel.hpp>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>

#include <algorithm>
#include <utility>

namespace fuse::animation {

#if defined(FUSE_HAS_CUDA)
/// kernels/skinning.cu: stages the mesh + palette on the device and runs the same kernel body.
bool launchSkinningCuda(const skinning_kernel::Params& params, void* stream);
#endif

bool skinning_cuda_kernel_available() {
#if defined(FUSE_HAS_CUDA)
    // kernels/skinning.cu is compiled whenever FUSE_HAS_CUDA is; a device must also be usable.
    return kernel::backend_available(kernel::Backend::Cuda);
#else
    return false;
#endif
}

SkinningBackend skinning_backend() {
    return skinning_cuda_kernel_available() ? SkinningBackend::Cuda : SkinningBackend::CpuReference;
}

bool skin_vertices_on(kernel::Backend backend, const SkinningInput& input, SkinningOutput& output, void* stream) {
    const usize vertexCount = input.rest_positions.size();
    const bool hasNormals = !input.rest_normals.empty();
    if (vertexCount == 0 || !std::in_range<u32>(vertexCount) || input.weights.size() != vertexCount ||
        (hasNormals && input.rest_normals.size() != vertexCount)) {
        return false;
    }

    output.positions.resize(vertexCount);
    output.normals.resize(hasNormals ? vertexCount : 0);
    const skinning_kernel::Params params = skinning_kernel::make_params(input, output);

#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) && skinning_cuda_kernel_available()) {
        return launchSkinningCuda(params, stream);
    }
#else
    (void)stream;
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend.
    return kernel::launch(backend, skinning_kernel::make_launch(params), skinning_kernel::Kernel{}, params).ok;
}

bool skin_vertices(const SkinningInput& input, SkinningOutput& output) {
    return skin_vertices_on(kernel::Backend::Auto, input, output);
}

void compute_skinning_palette(const Skeleton& skel, const Pose& pose, std::vector<mat4>& out) {
    const u32 count = std::min(static_cast<u32>(skel.bones.size()),
                               static_cast<u32>(pose.bone_world_transforms.size()));
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
        out[i] = mat4_multiply(pose.bone_world_transforms[i], skel.bones[i].inverse_bind);
    }
}

} // namespace fuse::animation
