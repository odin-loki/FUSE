// CUDA backend of the batched SVO ray cast: the __global__ trampoline from cuda_launch.cuh runs the
// same FUSE_HOST_DEVICE body (fuse/scene/svo_ray_kernel.hpp) the CPU backends run. This TU only stages
// the flat node / brick / payload arrays and the rays in device memory.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/scene/svo_ray_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::scene {

bool launchSvoRayCastCuda(const svo_kernel::Params& params, void* stream) {
    namespace sk = svo_kernel;
    if (!sk::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const u32 count = params.rays.size;

    kernel::cuda::DeviceBuffer<SVONode> nodes;
    kernel::cuda::DeviceBuffer<SVOBrick> bricks;
    kernel::cuda::DeviceBuffer<u32> pool;
    kernel::cuda::DeviceBuffer<SvoRay> rays;
    kernel::cuda::DeviceBuffer<SvoRayHit> hits;
    bool ok = nodes.allocate(params.svo.nodes.size) && bricks.allocate(params.svo.bricks.size) &&
              pool.allocate(params.svo.pool.size) && rays.allocate(count) && hits.allocate(count);
    ok = ok && nodes.upload(params.svo.nodes.data, params.svo.nodes.size, cudaStream) &&
         bricks.upload(params.svo.bricks.data, params.svo.bricks.size, cudaStream) &&
         pool.upload(params.svo.pool.data, params.svo.pool.size, cudaStream) &&
         rays.upload(params.rays.data, count, cudaStream);
    if (!ok) {
        return false;
    }

    sk::Params device = params;
    device.svo.nodes = kernel::make_span<const SVONode>(nodes.data(), params.svo.nodes.size);
    device.svo.bricks = kernel::make_span<const SVOBrick>(bricks.data(), params.svo.bricks.size);
    device.svo.pool = kernel::make_span<const u32>(pool.data(), params.svo.pool.size);
    device.rays = kernel::make_span<const SvoRay>(rays.data(), count);
    device.hits = kernel::make_span(hits.data(), count);

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<sk::Kernel, sk::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, sk::make_launch(count), sk::Kernel{}, device, options).ok;
    ok = ok && hits.download(params.hits.data, count, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::scene
