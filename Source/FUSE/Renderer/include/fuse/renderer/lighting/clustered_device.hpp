#pragma once

// Device-resident clustered light cull + deferred shade (B5.4, docs/compute-kernels.md). upload() stages the
// G-buffer view (device depth, world normals, albedo), the point lights and builds the cluster AABBs on the
// device once; every run() then executes the single-source kernels of fuse/renderer/lighting/clustered_kernel.hpp
// (the bodies the CPU backends run) on the resident data, on its own stream, timed with CUDA events:
//
//   clustered_light_bounds -> clustered_light_bin -> clustered_light_cull -> clustered_light_grid -> deferred_shading
//
// clustered_light_grid points the light grid at the cull's fixed-capacity lists in place (no host scan), so the
// device frame needs no host round trip; the lists and their order equal the CPU pipeline's
// (cullLightsToClusterLists + compactClusterLists), so the shade is the same computation.
//
// Available in FUSE_HAS_CUDA builds with a CUDA device (available()); anywhere else every call fails cleanly and
// message() says why. Device code: kernels/clustered_lighting.cu. No-CUDA stubs: src/lighting/clustered_shading.cpp.
// Gate / benchmark: tests/test_b5_clustered_device.cpp (fuse_b5_clustered_device).

#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_shading.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

/// CUDA-event timing of one device frame, in milliseconds on the frame's stream.
struct ClusteredDeviceTiming {
    f32 bounds_ms = 0.f; ///< clustered_light_bounds
    f32 bin_ms = 0.f;    ///< clustered_light_bin
    f32 cull_ms = 0.f;   ///< clustered_light_cull
    f32 grid_ms = 0.f;   ///< clustered_light_grid
    f32 shade_ms = 0.f;  ///< deferred_shading
    /// First -> last event: the whole clustered cull + deferred shade (counter reset included).
    f32 total_ms = 0.f;
};

class ClusteredDeviceFrame {
public:
    ClusteredDeviceFrame();
    ~ClusteredDeviceFrame();
    ClusteredDeviceFrame(const ClusteredDeviceFrame&) = delete;
    ClusteredDeviceFrame& operator=(const ClusteredDeviceFrame&) = delete;

    /// True in a FUSE_HAS_CUDA build with a usable CUDA device (kernel::backend_available(Backend::Cuda)).
    static bool available();

    /// Stages `gbuffer` (width x height depth / normals / albedo) and `lights` on the device, sizes the cull for
    /// ClusterDesc::clampCounts(desc) and builds the cluster AABBs (clustered_cluster_build, buildMs()).
    bool upload(const DeferredGBufferView& gbuffer,
                const ClusterDesc& desc,
                const ClusterCameraDesc& camera,
                const std::vector<PointLightInput>& lights);
    /// Replaces the resident point lights (same count or fewer than uploaded; lights move every frame).
    bool updateLights(const std::vector<PointLightInput>& lights);
    /// One clustered cull + deferred shade on the resident data (see the file comment), CUDA-event timed.
    bool run();
    /// The last run's radiance (width x height), shade counters and, optionally, its cull lists (bounds, slice
    /// bins, fixed-capacity slots, counts, drops — the ClusterCullLists layout of cullLightsToClusterLists).
    bool download(std::vector<math::Vec3>& radiance, DeferredShadeStats& stats, ClusterCullLists* lists = nullptr);

    /// On: every launch synchronises, so the kernel stats / profiler scopes of the five kernels carry
    /// GPU-inclusive durations (the profiler's pass breakdown). Off (default): asynchronous launches.
    void setSynchronousLaunches(bool on) { m_synchronous = on; }

    bool resident() const { return m_resident; }
    bool ok() const { return m_ok; }
    const std::string& message() const { return m_message; }
    const ClusteredDeviceTiming& lastTiming() const { return m_timing; }
    /// CUDA-event time of the cluster AABB build in upload().
    f32 buildMs() const { return m_build_ms; }

private:
    struct Impl;
    bool fail(const char* message);

    std::unique_ptr<Impl> m_impl;
    ClusteredDeviceTiming m_timing{};
    std::string m_message;
    f32 m_build_ms = 0.f;
    bool m_resident = false;
    bool m_synchronous = false;
    bool m_ok = false;
};

} // namespace fuse::renderer
