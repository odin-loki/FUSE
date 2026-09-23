// CUDA backend of the DDGI probe update: the __global__ trampolines from cuda_launch.cuh run the same
// FUSE_HOST_DEVICE trace + blend bodies (fuse/renderer/gi/ddgi_probe_kernel.hpp) the CPU backends run.
// This TU only stages the scene, ray set and probe atlases in device memory and reads the results back.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::renderer {

namespace {

template <typename T>
bool stage(kernel::cuda::DeviceBuffer<T>& buffer, const T* host, usize count, cudaStream_t stream) {
    return buffer.allocate(count) && (host == nullptr || buffer.upload(host, count, stream));
}

} // namespace

bool launchDdgiProbeUpdateCuda(const ddgi_kernel::TraceParams& trace,
                               const ddgi_kernel::BlendParams& blend,
                               u32 slots,
                               void* stream) {
    namespace dk = ddgi_kernel;
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const DDGIDesc& desc = trace.volume.desc;
    const u32 probes = trace.volume.probe_count;
    const usize irrTexels = static_cast<usize>(probes) * (desc.irradiance_res + 2u) * (desc.irradiance_res + 2u);
    const usize distTexels = static_cast<usize>(probes) * (desc.depth_res + 2u) * (desc.depth_res + 2u);

    kernel::cuda::DeviceBuffer<u32> indices;
    kernel::cuda::DeviceBuffer<math::Vec3> rayDirs;
    kernel::cuda::DeviceBuffer<DdgiCpuBox> boxes;
    kernel::cuda::DeviceBuffer<math::Vec3> irradiance;
    kernel::cuda::DeviceBuffer<math::Vec2> distance;
    kernel::cuda::DeviceBuffer<math::Vec3> radiance;
    kernel::cuda::DeviceBuffer<f32> hitDistance;
    kernel::cuda::DeviceBuffer<math::Vec3> irrTexelDirs;
    kernel::cuda::DeviceBuffer<math::Vec3> distTexelDirs;
    kernel::cuda::DeviceBuffer<u32> updateCounts;
    kernel::cuda::DeviceBuffer<math::Vec4> incoming;
    kernel::cuda::DeviceBuffer<u32> fastTexels;
    bool ok = stage(indices, trace.probe_indices.data, slots, cudaStream) &&
              stage(rayDirs, trace.ray_dirs.data, trace.ray_dirs.size, cudaStream) &&
              stage(boxes, trace.scene.boxes, trace.scene.box_count, cudaStream) &&
              stage(irradiance, trace.volume.irradiance, irrTexels, cudaStream) &&
              stage(distance, trace.volume.distance, distTexels, cudaStream) &&
              stage<math::Vec3>(radiance, nullptr, trace.out_radiance.size, cudaStream) &&
              stage<f32>(hitDistance, nullptr, trace.out_distance.size, cudaStream) &&
              stage(irrTexelDirs, blend.irradiance_texel_dirs.data, blend.irradiance_texel_dirs.size, cudaStream) &&
              stage(distTexelDirs, blend.distance_texel_dirs.data, blend.distance_texel_dirs.size, cudaStream) &&
              stage(updateCounts, blend.update_counts, probes, cudaStream) &&
              stage<math::Vec4>(incoming, nullptr, blend.incoming.size, cudaStream) &&
              stage(fastTexels, blend.fast_response_texels, 1u, cudaStream);
    if (!ok) {
        return false;
    }

    dk::TraceParams deviceTrace = trace;
    deviceTrace.probe_indices = {indices.data(), slots};
    deviceTrace.ray_dirs = {rayDirs.data(), trace.ray_dirs.size};
    deviceTrace.scene.boxes = boxes.data();
    deviceTrace.volume.irradiance = irradiance.data();
    deviceTrace.volume.distance = distance.data();
    deviceTrace.out_radiance = {radiance.data(), trace.out_radiance.size};
    deviceTrace.out_distance = {hitDistance.data(), trace.out_distance.size};

    dk::BlendParams deviceBlend = blend;
    deviceBlend.probe_indices = {indices.data(), slots};
    deviceBlend.ray_dirs = deviceTrace.ray_dirs;
    deviceBlend.radiance = {radiance.data(), trace.out_radiance.size};
    deviceBlend.distance = {hitDistance.data(), trace.out_distance.size};
    deviceBlend.irradiance_texel_dirs = {irrTexelDirs.data(), blend.irradiance_texel_dirs.size};
    deviceBlend.distance_texel_dirs = {distTexelDirs.data(), blend.distance_texel_dirs.size};
    deviceBlend.irradiance = irradiance.data();
    deviceBlend.distance_moments = distance.data();
    deviceBlend.update_counts = updateCounts.data();
    deviceBlend.incoming = {incoming.data(), blend.incoming.size};
    deviceBlend.fast_response_texels = fastTexels.data();

    kernel::LaunchOptions traceOptions{};
    traceOptions.cuda = &kernel::cuda::entry<dk::TraceKernel, dk::TraceParams>;
    traceOptions.stream = stream;
    traceOptions.allow_fallback = false;
    traceOptions.synchronize = false; // same stream: the blend launch is ordered after the trace
    kernel::LaunchOptions blendOptions = traceOptions;
    blendOptions.cuda = &kernel::cuda::entry<dk::BlendKernel, dk::BlendParams>;
    blendOptions.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, dk::make_trace_launch(trace.ray_dirs.size, slots), dk::TraceKernel{},
                        deviceTrace, traceOptions)
             .ok &&
         kernel::launch(kernel::Backend::Cuda, dk::make_blend_launch(slots), dk::BlendKernel{}, deviceBlend,
                        blendOptions)
             .ok;
    ok = ok && irradiance.download(blend.irradiance, irrTexels, cudaStream) &&
         distance.download(blend.distance_moments, distTexels, cudaStream) &&
         updateCounts.download(blend.update_counts, probes, cudaStream) &&
         fastTexels.download(blend.fast_response_texels, 1u, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::renderer
