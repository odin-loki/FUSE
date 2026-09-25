// CUDA backend of clustered light culling + deferred shading: the item trampolines run the same
// FUSE_HOST_DEVICE bodies (fuse/renderer/lighting/clustered_kernel.hpp) the CPU backends run. This TU
// only stages inputs in device memory and reads the outputs back.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::renderer {

namespace {

template <typename T>
bool stage(kernel::cuda::DeviceBuffer<T>& buffer, const T* host, usize count, cudaStream_t stream) {
    return buffer.allocate(count) && (host == nullptr || buffer.upload(host, count, stream));
}

kernel::LaunchOptions deviceOptions(kernel::DeviceEntryFn entry, void* stream, bool synchronize) {
    kernel::LaunchOptions options{};
    options.cuda = entry;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = synchronize;
    return options;
}

} // namespace

bool launchClusteredCullCuda(const clustered_kernel::BoundsParams& bounds,
                             const clustered_kernel::BinParams& bin,
                             const clustered_kernel::CullParams& cull,
                             void* stream) {
    namespace ck = clustered_kernel;
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const u32 lights = bounds.out_bounds.size;
    const u32 clusters = cull.out_counts.size;

    kernel::cuda::DeviceBuffer<PointLightInput> points;
    kernel::cuda::DeviceBuffer<SpotLightInput> spots;
    kernel::cuda::DeviceBuffer<ClusterLightBounds> lightBounds;
    kernel::cuda::DeviceBuffer<ClusterAABB> aabbs;
    kernel::cuda::DeviceBuffer<u32> sliceLights;
    kernel::cuda::DeviceBuffer<u32> sliceCounts;
    kernel::cuda::DeviceBuffer<u32> slots;
    kernel::cuda::DeviceBuffer<u32> counts;
    kernel::cuda::DeviceBuffer<u32> dropped;
    bool ok = stage(points, bounds.point_lights.data, bounds.point_lights.size, cudaStream) &&
              stage(spots, bounds.spot_lights.data, bounds.spot_lights.size, cudaStream) &&
              stage<ClusterLightBounds>(lightBounds, nullptr, lights, cudaStream) &&
              stage(aabbs, cull.aabbs.data, cull.aabbs.size, cudaStream) &&
              stage<u32>(sliceLights, nullptr, bin.out_lights.size, cudaStream) &&
              stage<u32>(sliceCounts, nullptr, bin.out_counts.size, cudaStream) &&
              stage<u32>(slots, nullptr, cull.out_lights.size, cudaStream) &&
              stage<u32>(counts, nullptr, clusters, cudaStream) && stage<u32>(dropped, nullptr, clusters, cudaStream);
    if (!ok) {
        return false;
    }

    ck::BoundsParams deviceBounds = bounds;
    deviceBounds.point_lights = {points.data(), bounds.point_lights.size};
    deviceBounds.spot_lights = {spots.data(), bounds.spot_lights.size};
    deviceBounds.out_bounds = {lightBounds.data(), lights};
    ck::BinParams deviceBin = bin;
    deviceBin.bounds = {lightBounds.data(), lights};
    deviceBin.out_lights = {sliceLights.data(), bin.out_lights.size};
    deviceBin.out_counts = {sliceCounts.data(), bin.out_counts.size};
    ck::CullParams deviceCull = cull;
    deviceCull.aabbs = {aabbs.data(), cull.aabbs.size};
    deviceCull.bounds = {lightBounds.data(), lights};
    deviceCull.slice_lights = {sliceLights.data(), bin.out_lights.size};
    deviceCull.slice_counts = {sliceCounts.data(), bin.out_counts.size};
    deviceCull.out_lights = {slots.data(), cull.out_lights.size};
    deviceCull.out_counts = {counts.data(), clusters};
    deviceCull.out_dropped = {dropped.data(), clusters};

    ok = kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kBoundsName, lights), ck::BoundsKernel{},
                        deviceBounds, deviceOptions(&kernel::cuda::entry<ck::BoundsKernel, ck::BoundsParams>, stream, false))
             .ok &&
         kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kBinName, bin.out_counts.size), ck::BinKernel{},
                        deviceBin, deviceOptions(&kernel::cuda::entry<ck::BinKernel, ck::BinParams>, stream, false))
             .ok &&
         kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kCullName, clusters), ck::CullKernel{},
                        deviceCull, deviceOptions(&kernel::cuda::entry<ck::CullKernel, ck::CullParams>, stream, true))
             .ok;
    ok = ok && lightBounds.download(bounds.out_bounds.data, lights, cudaStream) &&
         sliceLights.download(bin.out_lights.data, bin.out_lights.size, cudaStream) &&
         sliceCounts.download(bin.out_counts.data, bin.out_counts.size, cudaStream) &&
         slots.download(cull.out_lights.data, cull.out_lights.size, cudaStream) &&
         counts.download(cull.out_counts.data, clusters, cudaStream) &&
         dropped.download(cull.out_dropped.data, clusters, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

bool launchDeferredShadingCuda(const clustered_kernel::ShadeParams& params, void* stream) {
    namespace ck = clustered_kernel;
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(params.width) * params.height;

    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<math::Vec3> albedo;
    kernel::cuda::DeviceBuffer<PointLightInput> lights;
    kernel::cuda::DeviceBuffer<ClusterGridEntry> grid;
    kernel::cuda::DeviceBuffer<u32> lightList;
    kernel::cuda::DeviceBuffer<math::Vec3> radiance;
    kernel::cuda::DeviceBuffer<u32> counters; // shaded, skipped, evaluations lo, evaluations hi
    const u32 zero[4] = {0u, 0u, 0u, 0u};
    bool ok = stage(depth, params.device_depth, pixels, cudaStream) &&
              stage(normals, params.normals, pixels, cudaStream) && stage(albedo, params.albedo, pixels, cudaStream) &&
              stage(lights, params.lights.data, params.lights.size, cudaStream) &&
              stage(grid, params.cluster_grid.data, params.cluster_grid.size, cudaStream) &&
              stage(lightList, params.light_list.data, params.light_list.size, cudaStream) &&
              stage<math::Vec3>(radiance, nullptr, pixels, cudaStream) && stage(counters, zero, 4u, cudaStream);
    if (!ok) {
        return false;
    }

    ck::ShadeParams deviceParams = params;
    deviceParams.device_depth = depth.data();
    deviceParams.normals = normals.data();
    deviceParams.albedo = albedo.data();
    deviceParams.lights = {lights.data(), params.lights.size};
    deviceParams.cluster_grid = {grid.data(), params.cluster_grid.size};
    deviceParams.light_list = {lightList.data(), params.light_list.size};
    deviceParams.out_radiance = radiance.data();
    deviceParams.shaded_pixels = counters.data();
    deviceParams.skipped_pixels = counters.data() + 1;
    deviceParams.evaluations = counters.data() + 2;
    ok = kernel::launch(kernel::Backend::Cuda, ck::make_shade_launch(params.width, params.height), ck::ShadeKernel{},
                        deviceParams, deviceOptions(&kernel::cuda::entry<ck::ShadeKernel, ck::ShadeParams>, stream, true))
             .ok;
    u32 hostCounters[4] = {0u, 0u, 0u, 0u};
    ok = ok && radiance.download(params.out_radiance, pixels, cudaStream) &&
         counters.download(hostCounters, 4u, cudaStream) && cudaStreamSynchronize(cudaStream) == cudaSuccess;
    if (ok) {
        *params.shaded_pixels += hostCounters[0];
        *params.skipped_pixels += hostCounters[1];
        params.evaluations[0] = hostCounters[2];
        params.evaluations[1] = hostCounters[3];
    }
    return ok;
}

} // namespace fuse::renderer
