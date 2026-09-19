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

/// Why a CUDA probe-kernel resource preflight rejected the request (B5.6 deepen).
enum class ProbeKernelResourceRejectReason : u8 {
    None = 0,
    NullProbeWorldPositions,
    NullIrradianceAtlas,
    NullDepthAtlas,
    NullPrevIrradianceSurface,
    NullOutRadianceSurface,
};

/// Human-readable label for probe-kernel reject reasons (logging / tests).
const char* probeKernelRejectReasonLabel(ProbeKernelRejectReason reason);

/// Human-readable label for probe-kernel resource reject reasons (logging / tests).
const char* probeKernelResourceRejectReasonLabel(ProbeKernelResourceRejectReason reason);

/// Preflight guard before probe trace kernel launch.
bool canLaunchProbeTraceKernel(const DDGIKernelParams& params);
/// Diagnose why probe trace launch preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Preflight guard before probe blend kernel launch.
bool canLaunchProbeBlendKernel(const DDGIKernelParams& params);
/// Diagnose why probe blend launch preflight would reject.
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// True when all GPU resource pointers required for probe trace are populated.
bool hasProbeTraceGpuResources(const DDGIKernelParams& params);
/// True when all GPU resource pointers required for probe blend are populated.
bool hasProbeBlendGpuResources(const DDGIKernelParams& params);
/// Diagnose why probe-kernel resource preflight would reject.
bool tryValidateProbeKernelResources(const DDGIKernelParams& params,
                                     ProbeKernelResourceRejectReason& outReason);
/// Combined launch + resource preflight for full CUDA probe trace.
bool tryCanLaunchProbeTraceKernelWithResources(const DDGIKernelParams& params,
                                               ProbeKernelRejectReason& outLaunchReason,
                                               ProbeKernelResourceRejectReason& outResourceReason);
/// Combined launch + resource preflight for full CUDA probe blend.
bool tryCanLaunchProbeBlendKernelWithResources(const DDGIKernelParams& params,
                                               ProbeKernelRejectReason& outLaunchReason,
                                               ProbeKernelResourceRejectReason& outResourceReason);

/// Launch probe trace kernel — returns true on success (stub when CUDA unavailable).
bool launch_probe_trace_kernel(const DDGIKernelParams& params, void* cuda_stream);

/// Launch probe blend kernel — returns true on success (stub when CUDA unavailable).
bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream);

} // namespace fuse::renderer::gi
