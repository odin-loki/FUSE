// CUDA backend of linear blend skinning: the __global__ trampoline from cuda_launch.cuh runs the same
// FUSE_HOST_DEVICE body (fuse/animation/skinning_kernel.hpp) the CPU backends run. This TU only stages
// the mesh, weights and bone palette in device memory and reads the skinned vertices back.

#include <fuse/animation/skinning.hpp>
#include <fuse/animation/skinning_kernel.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::animation {

bool launchSkinningCuda(const skinning_kernel::Params& params, void* stream) {
    namespace sk = skinning_kernel;
    if (!sk::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize vertices = params.rest_positions.size;
    const usize normals = params.rest_normals.size;

    kernel::cuda::DeviceBuffer<vec3> restPositions;
    kernel::cuda::DeviceBuffer<vec3> restNormals;
    kernel::cuda::DeviceBuffer<SkinningWeights> weights;
    kernel::cuda::DeviceBuffer<mat4> bones;
    kernel::cuda::DeviceBuffer<vec3> outPositions;
    kernel::cuda::DeviceBuffer<vec3> outNormals;
    bool ok = restPositions.allocate(vertices) && restNormals.allocate(normals) && weights.allocate(vertices) &&
              bones.allocate(params.bones.size) && outPositions.allocate(vertices) && outNormals.allocate(normals);
    ok = ok && restPositions.upload(params.rest_positions.data, vertices, cudaStream) &&
         restNormals.upload(params.rest_normals.data, normals, cudaStream) &&
         weights.upload(params.weights.data, vertices, cudaStream) &&
         bones.upload(params.bones.data, params.bones.size, cudaStream);
    if (!ok) {
        return false;
    }

    sk::Params device = params;
    device.rest_positions.data = restPositions.data();
    device.rest_normals.data = restNormals.data();
    device.weights.data = weights.data();
    device.bones.data = bones.data();
    device.out_positions.data = outPositions.data();
    device.out_normals.data = outNormals.data();

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<sk::Kernel, sk::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, sk::make_launch(params), sk::Kernel{}, device, options).ok;
    ok = ok && outPositions.download(params.out_positions.data, vertices, cudaStream) &&
         outNormals.download(params.out_normals.data, normals, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::animation
