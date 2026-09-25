// CUDA backend of the SDF ray march: the __global__ trampoline from cuda_launch.cuh runs the same
// FUSE_HOST_DEVICE body (fuse/compute/ray_march_kernel.hpp) the CPU backends run. This TU only stages
// the scene and surfaces in device memory — the pattern for every ported kernel's CUDA host wrapper.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::compute {

bool launchRayMarchCuda(const RayMarchParams& params, void* stream) {
    namespace rm = ray_march_kernel;
    if (!rm::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(params.width) * params.height;

    kernel::cuda::DeviceBuffer<SdfObject> objects;
    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec4> normals;
    bool ok = objects.allocate(params.object_count) && objects.upload(params.objects, params.object_count, cudaStream);
    if (ok && params.depth_surface != nullptr) {
        ok = depth.allocate(pixels);
    }
    if (ok && params.output_surface != nullptr) {
        ok = normals.allocate(pixels);
    }
    if (!ok) {
        return false;
    }

    RayMarchParams deviceScene = params;
    deviceScene.objects = objects.data();
    deviceScene.depth_surface = params.depth_surface != nullptr ? depth.data() : nullptr;
    deviceScene.output_surface = params.output_surface != nullptr ? normals.data() : nullptr;

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<rm::Kernel, rm::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, rm::make_launch(params), rm::Kernel{}, rm::make_params(deviceScene),
                        options)
             .ok;
    if (ok && params.depth_surface != nullptr) {
        ok = depth.download(static_cast<f32*>(params.depth_surface), pixels, cudaStream);
    }
    if (ok && params.output_surface != nullptr) {
        ok = normals.download(static_cast<math::Vec4*>(params.output_surface), pixels, cudaStream);
    }
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::compute
