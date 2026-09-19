#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

struct DDGIDesc;

} // namespace fuse::renderer

namespace fuse::renderer::gi {

/// Why a CUDA kernel launch preflight rejected the request (B5.6 deepen).
enum class DdgiKernelRejectReason : u8 {
/// Why a DDGI kernel launch preflight rejected the request (B5.6 deepen).
enum class KernelLaunchRejectReason : u8 {
    None = 0,
    NullIndices,
    ZeroCount,
    ZeroRaysPerProbe,
};

/// Human-readable label for kernel reject reasons (logging / tests).
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason);
/// Why a DDGI kernel launch preflight rejected the request (B5.6 deepen).
enum class DdgiKernelLaunchRejectReason : u8 {
    ZeroUpdateCount,
    NullProbeIndices,

/// Human-readable label for kernel launch reject reasons (logging / tests).
const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason);
const char* kernelLaunchRejectReasonLabel(KernelLaunchRejectReason reason);

/// CUDA kernel parameter bundle for DDGI probe update (B5.6 — P5 §5.6).
///
/// Why a DDGI kernel launch preflight rejected the request (B5.6 deepen).
enum class DdgiKernelRejectReason : u8 {
    None = 0,
    NullIndices,
    ZeroCount,
    InvalidRaysPerProbe,
};

/// Human-readable label for kernel launch reject reasons (logging / tests).
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason);

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
    NullRadianceSurfaces,
    NullAtlasSurfaces,
    ZeroMaxRayDistance,
    InvalidHysteresis,
};

/// Why a CUDA probe-kernel resource preflight rejected the request (B5.6 deepen).
enum class ProbeKernelResourceRejectReason : u8 {
    None = 0,
    NullProbeWorldPositions,
    NullIrradianceAtlas,
    NullDepthAtlas,
    NullPrevIrradianceSurface,
    NullOutRadianceSurface,
    NullBlendSurfaces,
    NullRadianceSurface,
};

/// Human-readable label for probe-kernel reject reasons (logging / tests).
const char* probeKernelRejectReasonLabel(ProbeKernelRejectReason reason);
/// Classify why probe-kernel launch preflight would reject — same ordering as `tryCanLaunchProbeTraceKernel`.
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params);

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
/// Human-readable label for probe-kernel resource reject reasons (logging / tests).
const char* probeKernelResourceRejectReasonLabel(ProbeKernelResourceRejectReason reason);

/// Preflight guard before probe trace kernel launch.
bool canLaunchProbeTraceKernel(const DDGIKernelParams& params);
/// Early-out when probe trace launch would be rejected.
/// Early-out when probe trace launch would be rejected — same ordering as `canLaunchProbeTraceKernel`.
/// Early-out when probe trace kernel launch would be rejected.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);
/// Diagnose why probe trace launch preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
/// Early-out when probe trace launch would be rejected.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);

/// Preflight guard before probe blend kernel launch.
bool canLaunchProbeBlendKernel(const DDGIKernelParams& params);
/// Early-out when probe blend launch would be rejected.
/// Early-out when probe blend launch would be rejected — same ordering as `canLaunchProbeBlendKernel`.
/// Early-out when probe blend kernel launch would be rejected.
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params);
/// Diagnose why probe blend launch preflight would reject.
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);
/// Early-out when probe trace launch would be rejected — same ordering as `canLaunchProbeTraceKernel`.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);
/// Early-out when probe blend launch would be rejected — same ordering as `canLaunchProbeBlendKernel`.
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params);
/// Preflight guard before probe update launch — false when params or indices are unusable.
bool canLaunchProbeUpdate(const DDGIKernelParams& params);
/// Why a probe-kernel launch preflight rejected the request (B5.6 deepen).
enum class ProbeKernelLaunchRejectReason : u8 {
    ZeroProbeCount,

/// Human-readable label for probe-kernel launch reject reasons (logging / tests).
const char* probeKernelLaunchRejectReasonLabel(ProbeKernelLaunchRejectReason reason);

/// Preflight guard before probe trace/blend kernel launch.
bool canLaunchProbeKernels(const DDGIKernelParams& params);

/// Diagnose why probe-kernel launch preflight would reject the request.
bool tryCanLaunchProbeKernels(const DDGIKernelParams& params, ProbeKernelLaunchRejectReason& outReason);
/// Why a kernel launch preflight rejected the params bundle (B5.6 deepen).
enum class DdgiKernelLaunchRejectReason : u8 {
    NullIndices,

/// Human-readable label for kernel launch reject reasons (logging / tests).
const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason);

/// Diagnose why probe trace launch would reject.
bool preflightProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason);
/// Diagnose why probe blend launch would reject.
bool preflightProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason);

/// True when kernel params carry a non-zero probe index list for stub launch.

/// Why a DDGI kernel launch preflight rejected the request (B5.6 deepen).
enum class DdgiKernelRejectReason : u8 {
    ZeroCount,
    InvalidRaysPerProbe,
    OutOfRangeIndex,

/// Human-readable label for kernel reject reasons (logging / tests).
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason);

/// Preflight guard before probe trace kernel launch; false on null indices or zero count.
/// Diagnose why trace-kernel preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);

/// Preflight guard before probe blend kernel launch; false on null indices or zero count.
/// Diagnose why blend-kernel preflight would reject.
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);

/// Combined host+params preflight — validates indices against `desc` before kernel launch.
bool canLaunchDdgiKernels(const ::fuse::renderer::DDGIDesc& desc, const DDGIKernelParams& params);
/// Diagnose why combined kernel preflight would reject.
bool tryCanLaunchDdgiKernels(const ::fuse::renderer::DDGIDesc& desc,
                             const DDGIKernelParams& params,
                             DdgiKernelRejectReason& outReason);

/// Diagnose why probe trace preflight would reject; vacuously succeeds when launch is allowed.

/// Diagnose why probe blend preflight would reject; vacuously succeeds when launch is allowed.


/// Preflight guard before probe trace/blend kernel launch; false on invalid params.
bool canLaunchDdgiKernelParams(const DDGIKernelParams& params);
/// Diagnose why kernel launch preflight would reject; vacuously succeeds when launchable.
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason);
/// Why DDGI kernel launch preflight rejected the parameter bundle (B5.6 deepen).


/// Diagnose the first kernel parameter invariant that fails.
bool preflightDDGIKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);


/// Why DDGI kernel launch preflight rejected the request (B5.6 deepen).


/// Preflight guard before probe trace kernel launch (B5.6 deepen).
/// Preflight guard before probe blend kernel launch (B5.6 deepen).
/// Diagnose why kernel launch preflight would reject (B5.6 deepen).
bool preflightDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason);

/// Diagnose why probe trace kernel launch preflight would reject.

/// Diagnose why probe blend kernel launch preflight would reject.
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason);

bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason);

/// Diagnose extended kernel-parameter invariants (rays-per-probe, etc.) beyond launch guards.
bool preflightProbeKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Early-out when probe trace kernel launch would be rejected.
/// Early-out when probe blend kernel launch would be rejected.
/// Classify why probe-kernel launch preflight would reject — same ordering as trace preflight.
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params);
/// Early-out when probe trace kernel launch preflight would reject.
/// Early-out when probe blend kernel launch preflight would reject.

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

/// True when required blend-kernel output surfaces are bound (additive CUDA-path preflight).
bool hasProbeBlendKernelSurfaces(const DDGIKernelParams& params);
/// Diagnose missing blend-kernel surfaces without affecting stub launch paths.
bool tryValidateProbeBlendKernelSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Unified preflight guard before probe trace/blend kernel launch.
/// Diagnose why unified kernel launch preflight would reject.
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason);

/// Preflight guard before probe trace kernel launch including GPU surface pointers.
bool canLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params);
/// Diagnose why trace launch with GPU surfaces would reject.
bool tryCanLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params,
                                              ProbeKernelRejectReason& outReason);

/// Preflight guard before probe blend kernel launch including GPU atlas surfaces.
bool canLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params);
/// Diagnose why blend launch with GPU surfaces would reject.
bool tryCanLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params,

/// Early-out when probe blend launch would be rejected.
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params);

/// Early-out when probe trace launch would be rejected — same ordering as `canLaunchProbeTraceKernel`.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);
/// Early-out when probe blend launch would be rejected — same ordering as `canLaunchProbeBlendKernel`.

/// Resource preflight for probe trace — checks world-position buffer without launch-count guards.
bool tryPreflightProbeTraceKernelResources(const DDGIKernelParams& params,
                                           ProbeKernelRejectReason& outReason);
/// Resource preflight for probe blend — checks atlas surfaces without launch-count guards.
bool tryPreflightProbeBlendKernelResources(const DDGIKernelParams& params,
                                           ProbeKernelRejectReason& outReason);

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

/// Launch trace + blend kernels with reject-reason diagnostics; false when either preflight rejects.
bool tryLaunch_probe_kernels(const DDGIKernelParams& params,
/// Launch probe trace with reject-reason diagnostics; false when preflight rejects.
/// Early-out when probe trace launch would be rejected — same ordering as `canLaunchProbeTraceKernel`.
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params);

/// Launch probe blend with reject-reason diagnostics; false when preflight rejects.
/// Early-out when probe blend launch would be rejected — same ordering as `canLaunchProbeBlendKernel`.
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params);
                                  void* cuda_stream,
                                  ProbeKernelRejectReason& outReason);

} // namespace fuse::renderer::gi
