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

/// True when a kernel reject reason would block launch (B5.6 deepen pass).
bool probeKernelRejectReasonIsBlocking(ProbeKernelRejectReason reason);

/// Classify why probe kernel launch would reject — same ordering as `tryCanLaunchProbeTraceKernel`.
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params);

/// Early-out when either probe kernel launch would be rejected.
bool wouldSkipProbeKernelLaunch(const DDGIKernelParams& params);

/// Non-mutating kernel launch preflight — returns true when both kernels would proceed.
bool preflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);
/// Kernel launch preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Populate kernel params from desc + scheduled indices without changing launch guards.
void populateDDGIKernelParams(DDGIKernelParams& params,
                              const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              u64 frame_seed = 0);

/// Preflight guard before probe trace kernel launch.
bool canLaunchProbeTraceKernel(const DDGIKernelParams& params);
/// Diagnose why probe trace launch preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Preflight guard before probe blend kernel launch.
bool canLaunchProbeBlendKernel(const DDGIKernelParams& params);
/// Diagnose why probe blend launch preflight would reject.
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
/// Early-out when probe trace launch would be rejected — same ordering as `canLaunchProbeTraceKernel`.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);
/// Early-out when probe blend launch would be rejected — same ordering as `canLaunchProbeBlendKernel`.
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

/// Launch trace + blend kernels with reject-reason diagnostics; false when either preflight rejects.
bool tryLaunch_probe_kernels(const DDGIKernelParams& params,
                             void* cuda_stream,
                             ProbeKernelRejectReason& outReason);

} // namespace fuse::renderer::gi

// --- deepen additive from deepen-ddgi-b56-guards-5dea ---
enum class DdgiKernelRejectReason : u8 {
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason);
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);
bool tryCanLaunchDdgiKernels(const ::fuse::renderer::DDGIDesc& desc,
                             DdgiKernelRejectReason& outReason);

// --- deepen additive from ddgi-b56-guards-deepen-c1b1 ---
enum class DdgiKernelLaunchRejectReason : u8 {
const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason);
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason);

// --- deepen additive from deepen-ddgi-b56-guards-7061 ---
bool preflightDDGIKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);

// --- deepen additive from ddgi-probe-grid-guards-03fa ---
enum class KernelLaunchRejectReason : u8 {
const char* kernelLaunchRejectReasonLabel(KernelLaunchRejectReason reason);
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason);
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason);

// --- deepen additive from deepen-ddgi-probe-preflights-021e ---
bool preflightProbeKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from deepen-ddgi-b56-guards-be5b ---
enum class ProbeKernelResourceRejectReason : u8 {
const char* probeKernelResourceRejectReasonLabel(ProbeKernelResourceRejectReason reason);
bool tryValidateProbeKernelResources(const DDGIKernelParams& params,
                                     ProbeKernelResourceRejectReason& outReason);
bool tryCanLaunchProbeTraceKernelWithResources(const DDGIKernelParams& params,
                                               ProbeKernelRejectReason& outLaunchReason,
                                               ProbeKernelResourceRejectReason& outResourceReason);
bool tryCanLaunchProbeBlendKernelWithResources(const DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-guards-eb89 ---
bool tryValidateProbeBlendKernelSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from ddgi-b56-guards-deepen-0ebc ---
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from deepen-ddgi-guards-09bc ---
bool tryCanLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params,
bool tryCanLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-guards-c8ba ---
bool tryPreflightProbeTraceKernelResources(const DDGIKernelParams& params,
bool tryPreflightProbeBlendKernelResources(const DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-b56-guards-c107 ---
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);

// --- deepen additive from deepen-b56-ddgi-guards-9944 ---
ProbeKernelRejectReason classifyProbeTraceKernelReject(const DDGIKernelParams& params);
bool preflightProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);
ProbeKernelRejectReason classifyProbeBlendKernelReject(const DDGIKernelParams& params);
bool preflightProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);

// --- deepen additive from deepen-ddgi-guards-33be ---
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from deepen-ddgi-guards-4d4e ---
ProbeKernelRejectReason classifyProbeKernelTraceReject(const DDGIKernelParams& params);
ProbeKernelRejectReason classifyProbeKernelBlendReject(const DDGIKernelParams& params);
bool tryPreflightProbeTraceWorldPositions(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
bool tryPreflightProbeBlendSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from deepen-ddgi-guards-13d5 ---
bool tryPreflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason& reason);

// --- deepen additive from deepen-ddgi-b56-guards-ae4c ---
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params, const DDGIDesc& desc);
bool wouldSkipProbeKernelLaunch(const DDGIKernelParams& params, const DDGIDesc& desc);

// --- deepen additive from deepen-ddgi-guards-914b ---
bool preflightProbeTraceKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);
bool preflightProbeBlendKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason = nullptr);
bool populateAndPreflightDDGIKernelParams(DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-guards-51fd ---
bool tryPreflightProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
bool tryPreflightProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

// --- deepen additive from deepen-ddgi-b56-guards-132c ---
ProbeKernelRejectReason classifyProbeKernelRejectForDesc(const DDGIDesc& desc, const DDGIKernelParams& params);
bool preflightProbeKernelLaunchForDesc(const DDGIDesc& desc,
bool wouldSkipProbeKernelLaunchForDesc(const DDGIDesc& desc, const DDGIKernelParams& params);

// --- deepen additive from deepen-ddgi-guards-0e44 ---
bool preflightDdgiKernelUpdate(const DDGIDesc& desc,
bool wouldSkipDdgiKernelUpdate(const DDGIDesc& desc,
