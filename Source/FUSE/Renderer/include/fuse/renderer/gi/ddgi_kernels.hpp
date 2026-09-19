#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::gi {

/// CUDA kernel parameter bundle for DDGI probe update (B5.6 — P5 §5.6).
///
/// Full kernels (`probe_trace_kernel`, `probe_blend_kernel`) cast rays from each
/// scheduled probe, accumulate radiance, and blend into the irradiance atlas via
/// hysteresis. Implementation deferred — this header keeps the launch surface.
struct DDGIKernelParams {
    const u32* probe_indices_to_update = nullptr;
    u32 probe_update_count = 0;
    const void* probe_world_positions = nullptr;
    void* prev_irradiance_surface = nullptr;
    void* out_radiance_surface = nullptr;
    void* irradiance_atlas_surface = nullptr;
    void* depth_atlas_surface = nullptr;
    u32 rays_per_probe = 256;
    u64 frame_seed = 0;
    f32 hysteresis = 0.97f;
    f32 max_ray_distance = 20.f;
};

/// Why a CUDA probe-kernel launch preflight rejected the request (B5.6 deepen).
enum class ProbeKernelRejectReason : u8 {
    None = 0,
    ZeroUpdateCount,
    NullProbeIndices,
    ZeroRaysPerProbe,
};

/// Human-readable label for probe-kernel reject reasons (logging / tests).
const char* probeKernelRejectReasonLabel(ProbeKernelRejectReason reason);

/// Preflight guard before probe trace kernel launch.
bool canLaunchProbeTraceKernel(const DDGIKernelParams& params);
/// Diagnose why probe trace launch preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Preflight guard before probe blend kernel launch.
bool canLaunchProbeBlendKernel(const DDGIKernelParams& params);
/// Diagnose why probe blend launch preflight would reject.
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
/// Classify why probe-kernel launch preflight would reject — same ordering as trace preflight.
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params);
/// Early-out when probe trace kernel launch preflight would reject.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);
/// Early-out when probe blend kernel launch preflight would reject.
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params);

/// Launch probe trace kernel — returns true on success (stub when CUDA unavailable).
bool launch_probe_trace_kernel(const DDGIKernelParams& params, void* cuda_stream);
/// Launch probe trace kernel with reject-reason diagnostics; false when preflight rejects.
bool tryLaunch_probe_trace_kernel(const DDGIKernelParams& params,
                                  void* cuda_stream,
                                  ProbeKernelRejectReason& outReason);

/// Launch probe blend kernel — returns true on success (stub when CUDA unavailable).
bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream);
/// Launch probe blend kernel with reject-reason diagnostics; false when preflight rejects.
bool tryLaunch_probe_blend_kernel(const DDGIKernelParams& params,
                                  void* cuda_stream,
                                  ProbeKernelRejectReason& outReason);

} // namespace fuse::renderer::gi
