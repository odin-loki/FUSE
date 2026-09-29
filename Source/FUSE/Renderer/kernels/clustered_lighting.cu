// CUDA backend of clustered light culling + deferred shading: the item trampolines run the same
// FUSE_HOST_DEVICE bodies (fuse/renderer/lighting/clustered_kernel.hpp) the CPU backends run. This TU
// only stages inputs in device memory and reads the outputs back:
//   - launchClusteredCullCuda / launchDeferredShadingCuda: one-shot (the Cuda backend of
//     cullLightsToClusterLists / shadeDeferredFrame);
//   - ClusteredDeviceFrame (fuse/renderer/lighting/clustered_device.hpp): resident G-buffer / lights / lists,
//     bounds -> bin -> cull -> grid -> shade on one stream, timed with CUDA events.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/lighting/clustered_device.hpp>
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

// ---------------------------------------------------------------------------------------------
// ClusteredDeviceFrame (resident, CUDA-event timing)
// ---------------------------------------------------------------------------------------------

namespace {

f32 elapsedMs(cudaEvent_t from, cudaEvent_t to) {
    float ms = 0.f;
    return cudaEventElapsedTime(&ms, from, to) == cudaSuccess ? ms : 0.f;
}

} // namespace

struct ClusteredDeviceFrame::Impl {
    static constexpr u32 kEvents = 6u; // start, after bounds / bin / cull / grid / shade (upload reuses 0-1)

    cudaStream_t stream = nullptr;
    cudaEvent_t events[kEvents] = {};
    bool eventsReady = false;

    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<math::Vec3> albedo;
    kernel::cuda::DeviceBuffer<PointLightInput> lights;
    kernel::cuda::DeviceBuffer<ClusterAABB> aabbs;
    kernel::cuda::DeviceBuffer<ClusterLightBounds> bounds;
    kernel::cuda::DeviceBuffer<u32> sliceLights;
    kernel::cuda::DeviceBuffer<u32> sliceCounts;
    kernel::cuda::DeviceBuffer<u32> slots;
    kernel::cuda::DeviceBuffer<u32> counts;
    kernel::cuda::DeviceBuffer<u32> dropped;
    kernel::cuda::DeviceBuffer<ClusterGridEntry> grid;
    kernel::cuda::DeviceBuffer<math::Vec3> radiance;
    kernel::cuda::DeviceBuffer<u32> counters; // shaded, skipped, evaluations lo, evaluations hi

    clustered_kernel::GridDims dims{};
    clustered_kernel::CameraView camera{};
    u32 width = 0;
    u32 height = 0;
    u32 lightCount = 0;
    u32 lightCapacity = 0;
    u32 clusters = 0;
    u32 capacity = 0; ///< per-cluster list slots

    bool ensureStream() {
        if (stream == nullptr && cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
            stream = nullptr;
            return false;
        }
        if (!eventsReady) {
            for (cudaEvent_t& e : events) {
                if (cudaEventCreate(&e) != cudaSuccess) {
                    return false;
                }
            }
            eventsReady = true;
        }
        return true;
    }

    ~Impl() {
        if (stream != nullptr) {
            cudaStreamSynchronize(stream);
        }
        for (cudaEvent_t& e : events) {
            if (e != nullptr) {
                cudaEventDestroy(e);
            }
        }
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
    }
};

ClusteredDeviceFrame::ClusteredDeviceFrame() : m_impl(std::make_unique<Impl>()) {}
ClusteredDeviceFrame::~ClusteredDeviceFrame() = default;

bool ClusteredDeviceFrame::upload(const DeferredGBufferView& gbuffer,
                                  const ClusterDesc& desc,
                                  const ClusterCameraDesc& camera,
                                  const std::vector<PointLightInput>& lights) {
    namespace ck = clustered_kernel;
    m_resident = false;
    m_build_ms = 0.f;
    if (!available()) {
        return fail("clustered device frame: no CUDA device");
    }
    const ClusterDesc clamped = ClusterDesc::clampCounts(desc);
    const usize pixels = static_cast<usize>(gbuffer.width) * gbuffer.height;
    if (pixels == 0u || gbuffer.deviceDepth == nullptr || gbuffer.normals == nullptr || gbuffer.albedo == nullptr ||
        clamped.clusterCount() == 0u) {
        return fail("clustered device frame: empty G-buffer view or cluster grid");
    }
    Impl& im = *m_impl;
    if (!im.ensureStream()) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    im.dims = ck::make_grid(clamped);
    im.camera = ck::make_camera(camera);
    im.width = gbuffer.width;
    im.height = gbuffer.height;
    im.lightCount = static_cast<u32>(lights.size());
    im.lightCapacity = im.lightCount;
    im.clusters = clamped.clusterCount();
    im.capacity = clamped.maxLightsPerCluster > 0u ? std::min(clamped.maxLightsPerCluster, im.lightCount) : im.lightCount;
    const cudaStream_t s = im.stream;
    bool ok = im.depth.allocate(pixels) && im.depth.upload(gbuffer.deviceDepth, pixels, s) &&
              im.normals.allocate(pixels) && im.normals.upload(gbuffer.normals, pixels, s) &&
              im.albedo.allocate(pixels) && im.albedo.upload(gbuffer.albedo, pixels, s) &&
              im.lights.allocate(im.lightCount) && im.lights.upload(lights.data(), im.lightCount, s) &&
              im.aabbs.allocate(im.clusters) && im.bounds.allocate(im.lightCount) &&
              im.sliceLights.allocate(static_cast<usize>(im.dims.slices_z) * im.lightCount) &&
              im.sliceCounts.allocate(im.dims.slices_z) &&
              im.slots.allocate(static_cast<usize>(im.clusters) * im.capacity) && im.counts.allocate(im.clusters) &&
              im.dropped.allocate(im.clusters) && im.grid.allocate(im.clusters) && im.radiance.allocate(pixels) &&
              im.counters.allocate(4u);
    if (ok) {
        ck::BuildParams build{};
        build.grid = im.dims;
        build.camera = im.camera;
        build.out_aabbs = {im.aabbs.data(), im.clusters};
        kernel::LaunchOptions options{};
        options.cuda = &kernel::cuda::entry<ck::BuildKernel, ck::BuildParams>;
        options.stream = s;
        options.allow_fallback = false;
        options.synchronize = false;
        ok = cudaEventRecord(im.events[0], s) == cudaSuccess &&
             kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kBuildName, im.clusters), ck::BuildKernel{},
                            build, options)
                 .ok &&
             cudaEventRecord(im.events[1], s) == cudaSuccess && cudaStreamSynchronize(s) == cudaSuccess;
    }
    if (!ok) {
        const cudaError_t error = cudaGetLastError();
        return fail(error != cudaSuccess ? cudaGetErrorString(error) : "clustered device frame: upload failed");
    }
    m_build_ms = elapsedMs(im.events[0], im.events[1]);
    m_resident = true;
    m_ok = true;
    m_message.clear();
    return true;
}

bool ClusteredDeviceFrame::updateLights(const std::vector<PointLightInput>& lights) {
    Impl& im = *m_impl;
    if (!m_resident) {
        return fail("clustered device frame: upload() first");
    }
    if (lights.size() > im.lightCapacity) {
        return fail("clustered device frame: more lights than uploaded (upload() again)");
    }
    im.lightCount = static_cast<u32>(lights.size());
    const bool ok = im.lights.upload(lights.data(), im.lightCount, im.stream) &&
                    cudaStreamSynchronize(im.stream) == cudaSuccess;
    if (!ok) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    m_ok = true;
    m_message.clear();
    return true;
}

bool ClusteredDeviceFrame::run() {
    namespace ck = clustered_kernel;
    m_timing = {};
    if (!m_resident) {
        return fail("clustered device frame: upload() first");
    }
    Impl& im = *m_impl;
    const cudaStream_t s = im.stream;
    const u32 lights = im.lightCount;
    // Cull capacity follows the uploaded light count (the slot array is sized for it); fewer lights keep it.
    const u32 slices = im.dims.slices_z;

    ck::BoundsParams bounds{};
    bounds.grid = im.dims;
    bounds.camera = im.camera;
    bounds.point_lights = {im.lights.data(), lights};
    bounds.spot_lights = {};
    bounds.out_bounds = {im.bounds.data(), lights};

    ck::BinParams bin{};
    bin.bounds = {im.bounds.data(), lights};
    bin.capacity = lights;
    bin.out_lights = {im.sliceLights.data(), slices * lights};
    bin.out_counts = {im.sliceCounts.data(), slices};

    ck::CullParams cull{};
    cull.grid = im.dims;
    cull.capacity = im.capacity;
    cull.aabbs = {im.aabbs.data(), im.clusters};
    cull.bounds = {im.bounds.data(), lights};
    cull.slice_capacity = lights;
    cull.slice_lights = {im.sliceLights.data(), slices * lights};
    cull.slice_counts = {im.sliceCounts.data(), slices};
    cull.out_lights = {im.slots.data(), im.clusters * im.capacity};
    cull.out_counts = {im.counts.data(), im.clusters};
    cull.out_dropped = {im.dropped.data(), im.clusters};

    ck::GridParams grid{};
    grid.capacity = im.capacity;
    grid.counts = {im.counts.data(), im.clusters};
    grid.out_grid = {im.grid.data(), im.clusters};

    ck::ShadeParams shade{};
    shade.width = im.width;
    shade.height = im.height;
    shade.inv_width = 1.f / static_cast<f32>(im.width);
    shade.inv_height = 1.f / static_cast<f32>(im.height);
    shade.grid = im.dims;
    shade.camera = im.camera;
    shade.device_depth = im.depth.data();
    shade.normals = im.normals.data();
    shade.albedo = im.albedo.data();
    shade.lights = {im.lights.data(), lights};
    shade.cluster_grid = {im.grid.data(), im.clusters};
    shade.light_list = {im.slots.data(), im.clusters * im.capacity};
    shade.use_clusters = true;
    shade.out_radiance = im.radiance.data();
    shade.shaded_pixels = im.counters.data();
    shade.skipped_pixels = im.counters.data() + 1;
    shade.evaluations = im.counters.data() + 2;

    const bool sync = m_synchronous;
    bool ok = cudaEventRecord(im.events[0], s) == cudaSuccess &&
              cudaMemsetAsync(im.counters.data(), 0, 4u * sizeof(u32), s) == cudaSuccess &&
              kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kBoundsName, lights), ck::BoundsKernel{},
                             bounds, deviceOptions(&kernel::cuda::entry<ck::BoundsKernel, ck::BoundsParams>, s, sync))
                  .ok &&
              cudaEventRecord(im.events[1], s) == cudaSuccess &&
              kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kBinName, slices), ck::BinKernel{}, bin,
                             deviceOptions(&kernel::cuda::entry<ck::BinKernel, ck::BinParams>, s, sync))
                  .ok &&
              cudaEventRecord(im.events[2], s) == cudaSuccess &&
              kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kCullName, im.clusters), ck::CullKernel{},
                             cull, deviceOptions(&kernel::cuda::entry<ck::CullKernel, ck::CullParams>, s, sync))
                  .ok &&
              cudaEventRecord(im.events[3], s) == cudaSuccess &&
              kernel::launch(kernel::Backend::Cuda, ck::make_linear_launch(ck::kGridName, im.clusters), ck::GridKernel{},
                             grid, deviceOptions(&kernel::cuda::entry<ck::GridKernel, ck::GridParams>, s, sync))
                  .ok &&
              cudaEventRecord(im.events[4], s) == cudaSuccess &&
              kernel::launch(kernel::Backend::Cuda, ck::make_shade_launch(im.width, im.height), ck::ShadeKernel{}, shade,
                             deviceOptions(&kernel::cuda::entry<ck::ShadeKernel, ck::ShadeParams>, s, sync))
                  .ok &&
              cudaEventRecord(im.events[5], s) == cudaSuccess && cudaEventSynchronize(im.events[5]) == cudaSuccess;
    if (!ok) {
        const cudaError_t error = cudaGetLastError();
        return fail(error != cudaSuccess ? cudaGetErrorString(error) : "clustered device frame: launch failed");
    }
    m_timing.bounds_ms = elapsedMs(im.events[0], im.events[1]);
    m_timing.bin_ms = elapsedMs(im.events[1], im.events[2]);
    m_timing.cull_ms = elapsedMs(im.events[2], im.events[3]);
    m_timing.grid_ms = elapsedMs(im.events[3], im.events[4]);
    m_timing.shade_ms = elapsedMs(im.events[4], im.events[5]);
    m_timing.total_ms = elapsedMs(im.events[0], im.events[5]);
    m_ok = true;
    m_message.clear();
    return true;
}

bool ClusteredDeviceFrame::download(std::vector<math::Vec3>& radiance, DeferredShadeStats& stats,
                                    ClusterCullLists* lists) {
    if (!m_resident) {
        return fail("clustered device frame: upload() first");
    }
    Impl& im = *m_impl;
    const cudaStream_t s = im.stream;
    const usize pixels = static_cast<usize>(im.width) * im.height;
    radiance.resize(pixels);
    u32 counters[4] = {0u, 0u, 0u, 0u};
    bool ok = im.radiance.download(radiance.data(), pixels, s) && im.counters.download(counters, 4u, s);
    if (ok && lists != nullptr) {
        const u32 lights = im.lightCount;
        lists->capacity = im.capacity;
        lists->bounds.resize(lights);
        lists->sliceLights.resize(static_cast<usize>(im.dims.slices_z) * lights);
        lists->sliceCounts.resize(im.dims.slices_z);
        lists->slots.resize(static_cast<usize>(im.clusters) * im.capacity);
        lists->counts.resize(im.clusters);
        lists->dropped.resize(im.clusters);
        ok = im.bounds.download(lists->bounds.data(), lights, s) &&
             im.sliceLights.download(lists->sliceLights.data(), lists->sliceLights.size(), s) &&
             im.sliceCounts.download(lists->sliceCounts.data(), im.dims.slices_z, s) &&
             im.slots.download(lists->slots.data(), lists->slots.size(), s) &&
             im.counts.download(lists->counts.data(), im.clusters, s) &&
             im.dropped.download(lists->dropped.data(), im.clusters, s);
    }
    ok = ok && cudaStreamSynchronize(s) == cudaSuccess;
    if (!ok) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    stats.shadedPixels = counters[0];
    stats.skippedPixels = counters[1];
    stats.lightEvaluations = (static_cast<u64>(counters[3]) << 32u) | counters[2];
    m_ok = true;
    m_message.clear();
    return true;
}

} // namespace fuse::renderer
