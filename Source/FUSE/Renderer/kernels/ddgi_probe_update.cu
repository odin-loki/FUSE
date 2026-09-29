// CUDA backend of the DDGI probe update: the __global__ trampolines from cuda_launch.cuh run the same
// FUSE_HOST_DEVICE trace + blend bodies (fuse/renderer/gi/ddgi_probe_kernel.hpp) the CPU backends run.
// This TU only keeps the probe atlases, scene, ray set and scratch in device memory:
//   - launchDdgiProbeUpdateCuda: one-shot (DdgiCpuVolume with Backend::Cuda) — stage the atlases, run one
//     update, read them back;
//   - DdgiDeviceVolume (fuse/renderer/gi/ddgi_device.hpp): the atlases stay resident across updates, each
//     update uploads only the probe list / ray set / boxes and is timed with CUDA events.
// Both run the same DeviceState::run, so the two paths produce bit-identical atlases on one device.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/gi/ddgi_device.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::renderer {

namespace {

namespace dk = ddgi_kernel;

/// Grows `buffer` to at least `count` elements (contents are not preserved on growth).
template <typename T>
bool reserve(kernel::cuda::DeviceBuffer<T>& buffer, usize count) {
    return count <= buffer.size() || buffer.allocate(count);
}

/// Device copy of one probe volume plus one update's inputs and scratch.
struct DeviceState {
    kernel::cuda::DeviceBuffer<math::Vec3> irradiance;
    kernel::cuda::DeviceBuffer<math::Vec2> distance;
    kernel::cuda::DeviceBuffer<u32> update_counts;
    kernel::cuda::DeviceBuffer<math::Vec3> irradiance_dirs;
    kernel::cuda::DeviceBuffer<math::Vec3> distance_dirs;
    kernel::cuda::DeviceBuffer<u32> indices;
    kernel::cuda::DeviceBuffer<math::Vec3> ray_dirs;
    kernel::cuda::DeviceBuffer<DdgiCpuBox> boxes;
    kernel::cuda::DeviceBuffer<math::Vec3> radiance;
    kernel::cuda::DeviceBuffer<f32> hit_distance;
    kernel::cuda::DeviceBuffer<math::Vec4> incoming;
    kernel::cuda::DeviceBuffer<u32> fast_texels;
    u32 probes = 0;
    u32 irradiance_res = 0;
    u32 depth_res = 0;
    usize irradiance_texels = 0;
    usize distance_texels = 0;

    bool allocateVolume(const DDGIDesc& desc, u32 probe_count) {
        probes = probe_count;
        irradiance_res = desc.irradiance_res;
        depth_res = desc.depth_res;
        irradiance_texels = static_cast<usize>(probe_count) * (desc.irradiance_res + 2u) * (desc.irradiance_res + 2u);
        distance_texels = static_cast<usize>(probe_count) * (desc.depth_res + 2u) * (desc.depth_res + 2u);
        return irradiance.allocate(irradiance_texels) && distance.allocate(distance_texels) &&
               update_counts.allocate(probe_count) &&
               irradiance_dirs.allocate(static_cast<usize>(desc.irradiance_res) * desc.irradiance_res) &&
               distance_dirs.allocate(static_cast<usize>(desc.depth_res) * desc.depth_res) && fast_texels.allocate(1u);
    }

    bool matches(const dk::TraceParams& trace) const {
        return trace.volume.probe_count == probes && trace.volume.desc.irradiance_res == irradiance_res &&
               trace.volume.desc.depth_res == depth_res;
    }

    bool uploadVolume(const math::Vec3* irr,
                      const math::Vec2* dist,
                      const u32* counts,
                      kernel::Span<const math::Vec3> irr_dirs,
                      kernel::Span<const math::Vec3> dist_dirs,
                      cudaStream_t stream) {
        return irradiance.upload(irr, irradiance_texels, stream) && distance.upload(dist, distance_texels, stream) &&
               update_counts.upload(counts, probes, stream) &&
               irradiance_dirs.upload(irr_dirs.data, irr_dirs.size, stream) &&
               distance_dirs.upload(dist_dirs.data, dist_dirs.size, stream);
    }

    bool downloadVolume(math::Vec3* irr, math::Vec2* dist, u32* counts, cudaStream_t stream) const {
        return irradiance.download(irr, irradiance_texels, stream) && distance.download(dist, distance_texels, stream) &&
               update_counts.download(counts, probes, stream);
    }

    /// Stages the update's probe list, ray set and scene boxes, resets the fast-response counter and runs
    /// trace + blend on the resident atlases, all on `stream`. `events` (4, or null): recorded before the
    /// upload, after it, after the trace and after the blend. `synchronize`: every launch waits (profiling).
    bool run(const dk::TraceParams& trace,
             const dk::BlendParams& blend,
             u32 slots,
             cudaStream_t stream,
             bool synchronize,
             const cudaEvent_t* events) {
        if (!matches(trace)) {
            return false;
        }
        const u32 rays = trace.ray_dirs.size;
        const usize total = static_cast<usize>(slots) * rays;
        bool ok = reserve(indices, slots) && reserve(ray_dirs, rays) && reserve(boxes, trace.scene.box_count) &&
                  reserve(radiance, total) && reserve(hit_distance, total) && reserve(incoming, blend.incoming.size);
        if (!ok) {
            return false;
        }
        if (events != nullptr) {
            ok = cudaEventRecord(events[0], stream) == cudaSuccess;
        }
        ok = ok && indices.upload(trace.probe_indices.data, slots, stream) &&
             ray_dirs.upload(trace.ray_dirs.data, rays, stream) &&
             boxes.upload(trace.scene.boxes, trace.scene.box_count, stream) &&
             cudaMemsetAsync(fast_texels.data(), 0, sizeof(u32), stream) == cudaSuccess;
        if (ok && events != nullptr) {
            ok = cudaEventRecord(events[1], stream) == cudaSuccess;
        }
        if (!ok) {
            return false;
        }

        dk::TraceParams deviceTrace = trace;
        deviceTrace.probe_indices = {indices.data(), slots};
        deviceTrace.ray_dirs = {ray_dirs.data(), rays};
        deviceTrace.scene.boxes = boxes.data();
        deviceTrace.volume.irradiance = irradiance.data();
        deviceTrace.volume.distance = distance.data();
        deviceTrace.volume.probe_data = nullptr; // probe states are CPU-only
        deviceTrace.out_radiance = {radiance.data(), static_cast<u32>(total)};
        deviceTrace.out_distance = {hit_distance.data(), static_cast<u32>(total)};

        dk::BlendParams deviceBlend = blend;
        deviceBlend.probe_indices = deviceTrace.probe_indices;
        deviceBlend.ray_dirs = deviceTrace.ray_dirs;
        deviceBlend.radiance = {radiance.data(), static_cast<u32>(total)};
        deviceBlend.distance = {hit_distance.data(), static_cast<u32>(total)};
        deviceBlend.irradiance_texel_dirs = {irradiance_dirs.data(), blend.irradiance_texel_dirs.size};
        deviceBlend.distance_texel_dirs = {distance_dirs.data(), blend.distance_texel_dirs.size};
        deviceBlend.irradiance = irradiance.data();
        deviceBlend.distance_moments = distance.data();
        deviceBlend.update_counts = update_counts.data();
        deviceBlend.incoming = {incoming.data(), blend.incoming.size};
        deviceBlend.fast_response_texels = fast_texels.data();
        deviceBlend.probe_data = nullptr;

        kernel::LaunchOptions traceOptions{};
        traceOptions.cuda = &kernel::cuda::entry<dk::TraceKernel, dk::TraceParams>;
        traceOptions.stream = stream;
        traceOptions.allow_fallback = false;
        traceOptions.synchronize = synchronize; // same stream: the blend is ordered after the trace anyway
        kernel::LaunchOptions blendOptions = traceOptions;
        blendOptions.cuda = &kernel::cuda::entry<dk::BlendKernel, dk::BlendParams>;
        ok = kernel::launch(kernel::Backend::Cuda, dk::make_trace_launch(rays, slots), dk::TraceKernel{}, deviceTrace,
                            traceOptions)
                 .ok;
        if (ok && events != nullptr) {
            ok = cudaEventRecord(events[2], stream) == cudaSuccess;
        }
        ok = ok && kernel::launch(kernel::Backend::Cuda, dk::make_blend_launch(slots), dk::BlendKernel{}, deviceBlend,
                                  blendOptions)
                       .ok;
        if (ok && events != nullptr) {
            ok = cudaEventRecord(events[3], stream) == cudaSuccess;
        }
        return ok;
    }
};

f32 elapsedMs(cudaEvent_t from, cudaEvent_t to) {
    float ms = 0.f;
    return cudaEventElapsedTime(&ms, from, to) == cudaSuccess ? ms : 0.f;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// One-shot update (DdgiCpuVolume::updateProbes with Backend::Cuda / Auto)
// ---------------------------------------------------------------------------------------------

bool launchDdgiProbeUpdateCuda(const ddgi_kernel::TraceParams& trace,
                               const ddgi_kernel::BlendParams& blend,
                               u32 slots,
                               void* stream) {
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    DeviceState state;
    const bool ok = state.allocateVolume(trace.volume.desc, trace.volume.probe_count) &&
                    state.uploadVolume(trace.volume.irradiance, trace.volume.distance, blend.update_counts,
                                       blend.irradiance_texel_dirs, blend.distance_texel_dirs, cudaStream) &&
                    state.run(trace, blend, slots, cudaStream, false, nullptr) &&
                    state.downloadVolume(blend.irradiance, blend.distance_moments, blend.update_counts, cudaStream) &&
                    state.fast_texels.download(blend.fast_response_texels, 1u, cudaStream);
    return cudaStreamSynchronize(cudaStream) == cudaSuccess && ok;
}

// ---------------------------------------------------------------------------------------------
// DdgiDeviceVolume (resident atlases, CUDA-event timing)
// ---------------------------------------------------------------------------------------------

struct DdgiDeviceVolume::Impl {
    DeviceState state;
    cudaStream_t stream = nullptr;
    cudaEvent_t events[4] = {nullptr, nullptr, nullptr, nullptr};
    bool eventsReady = false;

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
        // The device buffers (cudaFree, not stream-ordered) are released after this body.
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
    }
};

DdgiDeviceVolume::DdgiDeviceVolume() : m_impl(std::make_unique<Impl>()) {}
DdgiDeviceVolume::~DdgiDeviceVolume() = default;

bool DdgiDeviceVolume::upload(const DdgiCpuVolume& volume) {
    m_resident = false;
    if (!available()) {
        return fail("DDGI device volume: no CUDA device");
    }
    if (!volume.isReady()) {
        return fail("DDGI device volume: the volume is not initialised");
    }
    if (volume.probeStatesEnabled()) {
        return fail("DDGI device volume: probe relocation / classification run on the CPU backends only");
    }
    Impl& im = *m_impl;
    if (!im.ensureStream()) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    const bool ok =
        im.state.allocateVolume(volume.desc(), volume.probeCount()) &&
        im.state.uploadVolume(volume.m_irradiance.data(), volume.m_distance.data(), volume.m_update_counts.data(),
                              {volume.m_scratch_texel_dirs.data(), static_cast<u32>(volume.m_scratch_texel_dirs.size())},
                              {volume.m_scratch_distance_dirs.data(),
                               static_cast<u32>(volume.m_scratch_distance_dirs.size())},
                              im.stream) &&
        cudaStreamSynchronize(im.stream) == cudaSuccess;
    if (!ok) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    m_resident = true;
    m_ok = true;
    m_message.clear();
    return true;
}

bool DdgiDeviceVolume::download(DdgiCpuVolume& volume) {
    if (!m_resident) {
        return fail("DDGI device volume: nothing uploaded");
    }
    Impl& im = *m_impl;
    if (volume.probeCount() != im.state.probes || volume.desc().irradiance_res != im.state.irradiance_res ||
        volume.desc().depth_res != im.state.depth_res) {
        return fail("DDGI device volume: download target does not match the uploaded volume");
    }
    const bool ok = im.state.downloadVolume(volume.m_irradiance.data(), volume.m_distance.data(),
                                            volume.m_update_counts.data(), im.stream) &&
                    cudaStreamSynchronize(im.stream) == cudaSuccess;
    if (!ok) {
        return fail(cudaGetErrorString(cudaGetLastError()));
    }
    m_ok = true;
    m_message.clear();
    return true;
}

bool DdgiDeviceVolume::launchOnDevice(DdgiUpdateLaunch& launch) {
    Impl& im = *m_impl;
    if (!im.state.matches(launch.trace)) {
        return fail("DDGI device update: the volume does not match the uploaded atlases (upload() again)");
    }
    const bool ok = im.state.run(launch.trace, launch.blend, launch.slots, im.stream, m_synchronous, im.events) &&
                    im.state.fast_texels.download(&launch.fast_response, 1u, im.stream) &&
                    cudaStreamSynchronize(im.stream) == cudaSuccess;
    if (!ok) {
        const cudaError_t error = cudaGetLastError();
        return fail(error != cudaSuccess ? cudaGetErrorString(error) : "DDGI device update: launch failed");
    }
    m_timing.upload_ms = elapsedMs(im.events[0], im.events[1]);
    m_timing.trace_ms = elapsedMs(im.events[1], im.events[2]);
    m_timing.blend_ms = elapsedMs(im.events[2], im.events[3]);
    m_timing.cycle_ms = elapsedMs(im.events[0], im.events[3]);
    return true;
}

} // namespace fuse::renderer
