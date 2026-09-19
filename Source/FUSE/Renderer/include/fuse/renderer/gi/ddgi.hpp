#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// 3D probe grid dimensions (B5.6 — P5 §5.6).
struct DDGIGridDims {
    u32 x = 16;
    u32 y = 8;
    u32 z = 16;
};

/// DDGI probe volume description — default 16×8×16 = 2048 probes.
struct DDGIDesc {
    fuse::math::Vec3 grid_origin{};
    fuse::math::Vec3 probe_spacing{2.f, 2.f, 2.f};
    DDGIGridDims grid_dims{};
    u32 rays_per_probe = 256;
    u32 probes_per_frame = 64;
    u32 irradiance_res = 8;
    u32 depth_res = 16;
    f32 hysteresis = 0.97f;
    f32 max_ray_distance = 20.f;
};

/// GPU irradiance cache handles for the probe volume.
struct ProbeVolume {
    TextureHandle irradiance_atlas{};
    TextureHandle depth_atlas{};
    BufferHandle probe_offsets{};
    u32 probe_count = 0;
};

/// Per-probe irradiance cache entry — CPU-side hysteresis scaffold for tests.
struct IrradianceCacheEntry {
    fuse::math::Vec3 irradiance{};
    f32 mean_depth = 0.f;
    f32 depth_variance = 0.f;
};

/// Combined probe data exposed to deferred shading kernels.
struct ProbeData {
    TextureHandle irradiance_atlas{};
    TextureHandle depth_atlas{};
    BufferHandle probe_offsets{};
    DDGIDesc desc{};
};

enum class DdgiBackend : u8 {
    Stub,
    CpuReference,
    Cuda,
};

struct DdgiInfo {
    bool valid = false;
    DdgiBackend backend = DdgiBackend::Stub;
    u32 probe_count = 0;
    const char* message = nullptr;
};

struct DDGIUpdateStats {
    u32 probes_scheduled = 0;
    u32 frame_index = 0;
    bool kernel_launched = false;
};

struct DDGISampleRequest {
    fuse::math::Vec3 world_position{};
    fuse::math::Vec3 world_normal{};
};

struct DDGISampleResult {
    fuse::math::Vec3 irradiance{};
    u32 nearest_probe = UINT32_MAX;
    bool valid = false;
};

/// Why probe sample-coord preflight rejected the request (B5.6 deepen).
enum class ProbeSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfBounds,
    InvalidWeights,
};

/// Human-readable label for probe sample-coord reject reasons (logging / tests).
const char* probeSampleRejectReasonLabel(ProbeSampleRejectReason reason);

/// Why probe cache-index lookup preflight rejected the request (B5.6 deepen).
enum class CacheLookupRejectReason : u8 {
    None = 0,
    EmptyGrid,
    UndersizedCache,
    ProbeIndexOutOfRange,
};

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheLookupRejectReasonLabel(CacheLookupRejectReason reason);

/// Why DDGI probe-update launch preflight rejected the request (B5.6 deepen).
enum class LaunchRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullIndices,
    ZeroCount,
    ProbeIndexOutOfRange,
    ZeroRaysPerProbe,
};

/// Human-readable label for launch reject reasons (logging / tests).
const char* launchRejectReasonLabel(LaunchRejectReason reason);

/// Integer probe coordinate within the 3D grid (B5.6 deepen).
struct ProbeGridCoord {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
};

/// Border shell classification for a probe cell (B5.6 deepen).
enum class ProbeBorderKind : u8 {
    Invalid,
    Interior,
    Face,
    Edge,
    Corner,
};

/// Per-probe validity flags for border/interior classification (B5.6 deepen).
struct ProbeValidityFlags {
    bool valid = false;
    bool is_border = false;
    bool interior = false;
    ProbeBorderKind border_kind = ProbeBorderKind::Invalid;
    /// True when the probe has a full 2×2×2 neighbourhood for trilinear sampling.
    bool has_trilinear_neighbourhood = false;
};

/// Why probe sample coord validation rejected a trilinear lookup (B5.6 deepen).
enum class ProbeSampleCoordsRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRangeIndices,
    OutOfRangeWeights,
    UnorderedCorners,
};

/// Human-readable label for sample-coords reject reasons (logging / tests).
const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason);

/// Why a cache-index guard rejected a probe lookup (B5.6 deepen).
enum class CacheIndexRejectReason : u8 {
    None = 0,
    EmptyGrid,
    ProbeIndexOutOfRange,
    CacheUndersized,
};

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);

/// Why a host probe-update launch preflight rejected the request (B5.6 deepen).
enum class DdgiLaunchRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullIndices,
    ZeroCount,
    OutOfRangeIndex,
};

/// Human-readable label for launch reject reasons (logging / tests).
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason);

/// Continuous octahedral tile sample coordinates for bilinear irradiance lookup.
struct DdgiTileBilinearCoords {
    u32 texel_u0 = 0;
    u32 texel_v0 = 0;
    u32 texel_u1 = 0;
    u32 texel_v1 = 0;
    f32 tu = 0.f;
    f32 tv = 0.f;
};

/// Why probe sample coord validation rejected a trilinear lookup (B5.6 deepen).
enum class ProbeSampleCoordsRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRangeIndices,
    OutOfRangeWeights,
    UnorderedCorners,
};

/// Human-readable label for sample-coords reject reasons (logging / tests).
const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason);

/// Why a cache-index guard rejected a probe lookup (B5.6 deepen).
enum class CacheIndexRejectReason : u8 {
    ProbeIndexOutOfRange,
    CacheUndersized,
/// Why probe sample-coord preflight rejected the request (B5.6 deepen).
enum class ProbeSampleCoordRejectReason : u8 {
    OutOfBounds,
    InvalidWeights,

/// Human-readable label for probe sample-coord reject reasons (logging / tests).
const char* probeSampleCoordRejectReasonLabel(ProbeSampleCoordRejectReason reason);

/// Why a probe cache-index preflight rejected the request (B5.6 deepen).
    OutOfRangeProbe,
    UndersizedCache,

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);

/// Why a host probe-update launch preflight rejected the request (B5.6 deepen).
enum class DdgiLaunchRejectReason : u8 {
    NullIndices,
    ZeroCount,
    OutOfRangeIndex,

/// Human-readable label for launch reject reasons (logging / tests).
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason);

/// Why probe sample coord build/sanitize preflight rejected (B5.6 deepen).
    InvalidSpacing,

/// Human-readable label for probe sample coord reject reasons (logging / tests).

/// Why DDGI probe-update launch preflight rejected the request (B5.6 deepen).
    None = 0,
    EmptyGrid,
};

/// Human-readable label for probe-update launch reject reasons (logging / tests).

/// Continuous probe-grid sample coordinates for trilinear irradiance lookup.
struct ProbeSampleCoords {
    u32 x0 = 0;
    u32 y0 = 0;
    u32 z0 = 0;
    u32 x1 = 0;
    u32 y1 = 0;
    u32 z1 = 0;
    f32 tx = 0.f;
    f32 ty = 0.f;
    f32 tz = 0.f;
};

/// Why probe-cache lookup preflight rejected the request (B5.6 deepen).
enum class ProbeCacheLookupRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullCache,
    UndersizedCache,
};

/// Human-readable label for cache lookup reject reasons (logging / tests).
const char* probeCacheLookupRejectReasonLabel(ProbeCacheLookupRejectReason reason);

/// Why DDGI probe-update launch preflight rejected the request (B5.6 deepen).
enum class DdgiLaunchRejectReason : u8 {
    NullIndices,
    ZeroProbeCount,
    InvalidRaysPerProbe,

/// Human-readable label for launch reject reasons (logging / tests).
/// Why probe sample coord build/validation rejected the request (B5.6 deepen).
enum class ProbeSampleCoordsRejectReason : u8 {
    InvalidSpacing,
    OutOfRangeIndices,
    UnorderedCorners,
    InvalidWeights,

/// Human-readable label for probe sample coord reject reasons (logging / tests).
const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason);

/// Why cache index lookup rejected the request (B5.6 deepen).
enum class CacheIndexRejectReason : u8 {
    ProbeIndexOutOfRange,
    CacheUndersized,

/// Human-readable label for cache index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);

/// Why probe-update launch preflight rejected the request (B5.6 deepen).
    NullIndexBuffer,
    OutOfRangeIndex,

/// Human-readable label for probe-update launch reject reasons (logging / tests).
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason);

/// Per-kind probe counts for border shell classification (B5.6 deepen).
struct ProbeBorderCounts {
    u32 total = 0;
    u32 interior = 0;
    u32 border = 0;
    u32 face = 0;
    u32 edge = 0;
    u32 corner = 0;
};

/// Why probe sample coord validation rejected the request (B5.6 deepen).
enum class ProbeSampleCoordsRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NotSampleableGrid,
    InvalidSpacing,
    NotSampleable,
    NonSampleableGrid,
    OutOfRangeIndices,
    OutOfRangeWeights,
    UnorderedCorners,
    UndersizedCache,
};

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason);
/// Classify why sample-coord validation would reject — same ordering as `tryValidateProbeSampleCoords`.
ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                              const ProbeSampleCoords& coords);
/// Early-out when sample-coord validation would reject.
bool shouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);

/// True when a sample-coord reject reason would block sampling (B5.6 deepen pass).
bool probeSampleCoordsRejectReasonIsBlocking(ProbeSampleCoordsRejectReason reason);

/// Why probe-grid source preflight rejected the request (B5.6 deepen pass).
enum class ProbeGridSourceRejectReason : u8 {
    NotSampleable,

/// Human-readable label for probe-grid source reject reasons (logging / tests).
const char* probeGridSourceRejectReasonLabel(ProbeGridSourceRejectReason reason);

/// True when a probe-grid source reject reason would block sampling (B5.6 deepen pass).
bool probeGridSourceRejectReasonIsBlocking(ProbeGridSourceRejectReason reason);
/// Classify why sample-coord validation would reject; vacuously returns `None` on valid coords (B5.6 deepen).
/// Classify why sample-coord validation would reject; vacuously `None` on valid coords (B5.6 deepen).

/// Classify why probe sample coord validation would reject (B5.6 deepen).

/// Diagnose why sample-coord validation would reject — no side effects (B5.6 deepen).
/// True when `coords` would be modified by `clampProbeSampleCoords` (B5.6 deepen).
bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
/// Classify why sample-coord validation would reject; returns `None` on valid coords (B5.6 deepen).
ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc, const ProbeSampleCoords& coords);

/// Why a cache-index lookup preflight rejected the request (B5.6 deepen).
enum class CacheIndexRejectReason : u8 {
    OutOfRangeProbeIndex,
/// Why a probe irradiance lookup would bail before trilinear sampling (B5.6 deepen).
enum class ProbeSampleSkipReason : u8 {
    ZeroIrradianceResolution,
    InvalidProbeSpacing,
    UndersizedCache,
    NullCache,
};

/// Why coord-based probe trilinear sampling preflight rejected the request (B5.6 deepen).
enum class ProbeTrilinearSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InvalidSampleCoords,

/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* probeTrilinearSampleRejectReasonLabel(ProbeTrilinearSampleRejectReason reason);

/// True when a trilinear sample reject reason would block sampling (B5.6 deepen pass).
bool probeTrilinearSampleRejectReasonIsBlocking(ProbeTrilinearSampleRejectReason reason);
    NotSampleable,
    OutOfRangeProbeIndex,
    ZeroCache,
    UndersizedCache,
};

/// Classify why coord-based probe trilinear sampling preflight would reject.
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count);

/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightTrilinearProbeSample(const DDGIDesc& desc,
                                   u32 cache_count,
                                   ProbeTrilinearSampleRejectReason* reason = nullptr);

/// Trilinear sample preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightTrilinearProbeSample(const DDGIDesc& desc,
                                      ProbeTrilinearSampleRejectReason& reason);

/// Early-out when coord-based probe trilinear sampling would be rejected (B5.6 deepen pass).
bool shouldSkipTrilinearProbeSample(const DDGIDesc& desc,
/// True when a trilinear sample reject reason would block lookup (B5.6 deepen pass).

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);

/// True when a cache-index reject reason would block lookup (B5.6 deepen pass).
bool cacheIndexRejectReasonIsBlocking(CacheIndexRejectReason reason);
/// Why spatial probe irradiance sampling preflight rejected the request (B5.6 deepen).
enum class ProbeSpatialSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InvalidSampleCoords,
    UndersizedCache,
    NullCache,
};

/// Human-readable label for spatial sample reject reasons (logging / tests).
const char* probeSpatialSampleRejectReasonLabel(ProbeSpatialSampleRejectReason reason);
/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* probeTrilinearSampleRejectReasonLabel(ProbeTrilinearSampleRejectReason reason);
/// Classify why coord-based probe sample preflight would reject — same ordering as `tryCanSampleAtProbeCoords`.
/// Classify why coord-based probe trilinear sampling preflight would reject (B5.6 deepen).
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count);
/// Classify why coord-based probe sample preflight would reject (B5.6 deepen).
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(
    const DDGIDesc& desc,

/// Why probe scheduling preflight rejected the request (B5.6 deepen).
/// Diagnose why coord-based probe trilinear sampling preflight would reject (B5.6 deepen).
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count);
/// Early-out when coord-based trilinear sampling would be rejected (B5.6 deepen).
bool shouldSkipTrilinearProbeSample(const DDGIDesc& desc,

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);
/// Classify why cache-index preflight would reject — same ordering as `tryValidateCacheIndex`.
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Classify cache-index preflight including null-cache rejection.
/// Classify why cache-index preflight would reject; vacuously returns `None` on valid indices (B5.6 deepen).
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                u32 probe_index,
/// Classify why cache-index preflight would reject (B5.6 deepen).

/// Classify why cache-index preflight would reject; returns `None` on valid indices (B5.6 deepen).
/// Classify cache-index preflight including null-cache rejection (B5.6 deepen).
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
                                                u32 cache_count);

/// Diagnose why cache-index preflight would reject — no side effects (B5.6 deepen).
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                u32 probe_index,
                                                u32 cache_count);
/// Diagnose cache-index preflight including null-cache rejection (B5.6 deepen).
                                                const IrradianceCacheEntry* cache,

/// Why a host probe-update launch preflight rejected the request (B5.6 deepen).
enum class ProbeUpdateLaunchRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullIndices,
    ZeroCount,
    OutOfRangeProbeIndex,
    DuplicateProbeIndex,
    ZeroRaysPerProbe,
};

/// Human-readable label for probe-update launch reject reasons (logging / tests).
const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason);
/// Classify why probe-update launch preflight would reject — same ordering as `tryCanLaunchDdgiProbeUpdate`.
/// Classify why probe-update launch preflight would reject; vacuously returns `None` when launchable (B5.6 deepen).
/// Classify why probe-update launch preflight would reject (B5.6 deepen).
ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count);

/// Classify why host probe-update launch preflight would reject (B5.6 deepen).

/// Diagnose why probe-update launch preflight would reject — no side effects (B5.6 deepen).
/// Classify why probe-update launch preflight would reject; returns `None` when launchable (B5.6 deepen).
ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count);

/// Why a DDGI irradiance sample request preflight rejected the request (B5.6 deepen).
enum class SampleRequestRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NotSampleable,
    UndersizedCache,
};

/// Human-readable label for sample-request reject reasons (logging / tests).
const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason);

/// Why probe-update scheduling preflight rejected the request (B5.6 deepen).
enum class ProbeScheduleRejectReason : u8 {
    NullOutput,
    ZeroProbeCount,
    ZeroMaxIndices,

/// Human-readable label for probe schedule reject reasons (logging / tests).
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason);

/// Why probe scheduling preflight rejected the request (B5.6 deepen).
enum class ProbeScheduleRejectReason : u8 {
    None = 0,
    NullOutputIndices,
    NullOutputCount,
    ZeroProbeCount,
    ZeroMaxIndices,
};

/// Why probe scheduling preflight rejected the request (B5.6 deepen).
enum class ProbeScheduleRejectReason : u8 {
    None = 0,
    NullOutputIndices,
    NullOutputCount,
    ZeroProbeCount,
    ZeroMaxIndices,
    ZeroProbesPerFrame,
    NullOutIndices,
    NullOutCount,
};

/// Human-readable label for probe-schedule reject reasons (logging / tests).
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason);
/// Classify why probe scheduling preflight would reject — same ordering as `tryCanScheduleProbeUpdates`.
/// Classify why probe scheduling preflight would reject; vacuously returns `None` when schedulable (B5.6 deepen).
/// Classify why probe scheduling preflight would reject (B5.6 deepen).
ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count);

/// Why a sample-request preflight rejected the request (B5.6 deepen).
enum class SampleRequestRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NotSampleable,
    UndersizedCache,
};

/// Human-readable label for sample-request reject reasons (logging / tests).
const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason);

/// Why probe scheduling preflight rejected the request (B5.6 deepen).
enum class ProbeScheduleRejectReason : u8 {
    NullIndices,
    NullCount,
    ZeroProbeCount,
    ZeroMaxIndices,
    ZeroProbesPerFrame,

/// Human-readable label for probe-schedule reject reasons (logging / tests).
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason);

/// Why probe round-robin scheduling preflight rejected the request (B5.6 deepen).


    NullOutputIndices,
    NullOutputCount,


    NullOutputBuffer,




/// Why probe-update scheduling preflight rejected the request (B5.6 deepen).
    NullOutput,




/// Why a host probe-update launch preflight rejected the request (B5.6 deepen).
enum class ProbeUpdateLaunchRejectReason : u8 {
    ZeroCount,


    OutOfRangeProbeIndex,
    ZeroRaysPerProbe,

/// Human-readable label for probe schedule reject reasons (logging / tests).

/// Human-readable label for probe-update launch reject reasons (logging / tests).
const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason);

/// True when a launch reject reason would block probe update (B5.6 deepen pass).
bool probeUpdateLaunchRejectReasonIsBlocking(ProbeUpdateLaunchRejectReason reason);

/// Classify why probe-update launch would reject — same ordering as `tryCanLaunchDdgiProbeUpdate`.
ProbeUpdateLaunchRejectReason classifyDdgiProbeUpdateReject(const DDGIDesc& desc,
                                                            const u32* probe_indices,
                                                            u32 probe_count);

/// Non-mutating host launch preflight — returns true when launch would proceed.
bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason = nullptr);
/// Host launch preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightDdgiProbeUpdate(const DDGIDesc& desc,
                                 ProbeUpdateLaunchRejectReason& outReason);
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 ProbeUpdateLaunchRejectReason& reason);
/// Early-out when probe-update launch would be rejected (B5.6 deepen pass).
/// Early-out when host probe-update launch would be rejected (B5.6 deepen pass).
bool shouldSkipDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);

    NullOutIndices,
    NullOutCount,
    NullIndicesBuffer,
    NullCountOutput,


/// True when a schedule reject reason would block probe scheduling (B5.6 deepen pass).
bool probeScheduleRejectReasonIsBlocking(ProbeScheduleRejectReason reason);
/// Human-readable label for probe sample skip reasons (logging / tests).
const char* probeSampleSkipReasonLabel(ProbeSampleSkipReason reason);

/// True when a skip reason prevents probe irradiance lookup.
bool probeSampleSkipReasonIsBlocking(ProbeSampleSkipReason reason);
/// Why a DDGI irradiance sample request preflight rejected the request (B5.6 deepen).
/// Why a DDGI sample request preflight rejected the request (B5.6 deepen).




/// Diagnose why probe scheduling preflight would reject — no side effects (B5.6 deepen).
/// Classify why probe scheduling preflight would reject; returns `None` when schedulable (B5.6 deepen).
ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count);

/// CPU-side octahedral direction encoding for probe irradiance atlas tiles (B5.6 deepen).
/// Mirrors `GBufferEncoding` and the deferred-shade probe sampling path.
struct DdgiIrradianceEncoding {
    static bool isEmptyDirection(const fuse::math::Vec3& direction);
    /// Inverse of `isEmptyDirection` — true when `direction` has non-zero length².
    static bool isValidDirection(const fuse::math::Vec3& direction);
    /// Normalizes `direction`, or `fallback` when empty; +Y when both are degenerate.
    static fuse::math::Vec3 resolveSampleDirection(const fuse::math::Vec3& direction,
                                                   const fuse::math::Vec3& fallback = {0.f, 1.f, 0.f});
    /// Resolve `direction`, then `surface_normal`, then +Y — mirrors deferred-shade GI sampling.
    static fuse::math::Vec3 resolveSampleDirectionFromSurface(const fuse::math::Vec3& direction,
                                                              const fuse::math::Vec3& surface_normal);
    static fuse::math::Vec2 encodeDirection(const fuse::math::Vec3& direction);
    static fuse::math::Vec3 decodeDirection(const fuse::math::Vec2& encoded);
    /// Clamp encoded octahedral UV to the unit square before decode/atlas lookup.
    static fuse::math::Vec2 clampEncodedUV(const fuse::math::Vec2& encoded);
    /// Unit-square UV within a probe's octahedral tile.
    static fuse::math::Vec2 directionToAtlasUV(const fuse::math::Vec3& direction);
    /// Texel offset within a probe tile — clamped to [0, irradiance_res - 1].
    static fuse::math::Vec2 directionToTexelOffset(const fuse::math::Vec3& direction, u32 irradiance_res);
    /// Fractional texel coordinates for bilinear octahedral tile sampling.
    static bool buildTileBilinearCoords(const fuse::math::Vec3& direction,
                                        u32 irradiance_res,
                                        DdgiTileBilinearCoords& out_coords);
    /// True when octahedral tile bilinear sampling is allowed for `irradiance_res`.
    static bool canDirectionallySample(u32 irradiance_res);
    static f32 angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b);
};

/// Probe grid indexing + atlas layout helpers — mirrors deferred-shade probe sampling.
struct ProbeGridLayout {
    static bool isEmptyGrid(const DDGIDesc& desc);
    static ProbeGridCoord probeCoordFromIndex(const DDGIDesc& desc, u32 probe_index);
    /// Decode a clamped flat probe index to grid coordinates; returns origin on empty grid.
    static ProbeGridCoord probeCoordFromClampedIndex(const DDGIDesc& desc, u32 probe_index);
    static u32 probeIndexFromCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static bool isValidProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// True when grid coords exceed grid bounds (would be clamped).
    static bool isProbeGridCoordOutOfRange(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// True when grid coordinates exceed bounds (would be clamped).
    /// True when tile coords exceed grid bounds (would be clamped).
    /// True when `coord` exceeds grid bounds (would be clamped); vacuously true on empty grid.
    static bool isProbeCoordOutOfRange(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static bool isValidProbeIndex(const DDGIDesc& desc, u32 probe_index);
    static bool isBorderProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Border probe check via flat index; false when index is out of range or grid is empty.
    static bool isBorderProbeIndex(const DDGIDesc& desc, u32 probe_index);
    /// Face/edge/corner shell classification; `Invalid` when coord or grid is empty.
    static ProbeBorderKind probeBorderKind(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Border kind via flat index; `Invalid` when index is out of range or grid is empty.
    static ProbeBorderKind probeBorderKindFromIndex(const DDGIDesc& desc, u32 probe_index);
    static ProbeValidityFlags probeValidity(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static ProbeValidityFlags probeValidityFromIndex(const DDGIDesc& desc, u32 probe_index);
    /// Validity for a flat probe index after `clampProbeIndex` (safe for OOB scheduling).
    static ProbeValidityFlags probeValidityFromClampedIndex(const DDGIDesc& desc, u32 probe_index);
    /// True when `probe_index` exceeds the valid probe range (would be clamped).
    static bool isProbeIndexOutOfRange(u32 probe_index, const DDGIDesc& desc);
    /// Last valid flat probe index; returns 0 when the grid is empty.
    static u32 maxProbeIndex(const DDGIDesc& desc);
    /// Last valid flat probe index; returns 0 when the grid has no probes.
    /// True when `probe_index` equals the last valid probe index for a non-empty grid.
    static bool isAtMaxProbeIndex(u32 probe_index, const DDGIDesc& desc);
    /// Clamp a flat probe index to [0, probeCount - 1]; returns 0 when the grid is empty.
    static u32 clampProbeIndex(u32 probe_index, const DDGIDesc& desc);
    /// Last valid flat probe index; returns 0 when the grid has no probes.
    static u32 maxProbeIndex(const DDGIDesc& desc);
    /// True when `probe_index` equals the last valid probe index for a non-empty grid.
    static bool isAtMaxProbeIndex(u32 probe_index, const DDGIDesc& desc);
    /// Clamp `probe_index` into range; returns false and zeroes `out_index` on an empty grid.
    static bool tryClampProbeIndex(u32 probe_index, const DDGIDesc& desc, u32& out_index);
    static u32 clampProbeCoordX(u32 x, const DDGIDesc& desc);
    static u32 clampProbeCoordY(u32 y, const DDGIDesc& desc);
    static u32 clampProbeCoordZ(u32 z, const DDGIDesc& desc);
    /// Clamp each axis then encode a flat probe index; returns 0 on empty grid.
    static u32 probeIndexFromClampedCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Clamp trilinear corner indices/weights to grid bounds (no-op on empty grid).
    static void clampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// True when corner indices and interpolation weights lie within grid bounds.
    static bool areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when interpolation weights or corner indices would be clamped before sampling.
    static bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight; false on empty desc or hard OOB corners.
    static bool tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                              const ProbeSampleCoords& coords,
                                              ProbeSampleCoordsRejectReason& outReason);
    /// Grid-only sample-coord preflight without reject-reason diagnostics.
    static bool canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight — indices and weights in range, ignores corner ordering.
    /// True when clamp or normalize would change the sample coords.
    /// Clamp sample coords in place; returns false without modifying `coords` on an empty grid.
    static bool tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// Clamp sample coords with reject-reason diagnostics.
    static bool tryClampProbeSampleCoords(const DDGIDesc& desc,
                                          ProbeSampleCoords& coords,
                                          ProbeSampleCoordsRejectReason& outReason);
    /// Ensure corner indices are ordered (x0≤x1, …) and weights stay in [0, 1].
    static void normalizeProbeSampleCoords(ProbeSampleCoords& coords);
    /// Normalize then validate; returns false without modifying `coords` on an empty grid.
    static bool tryNormalizeAndValidateProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// True when corner indices lie within the grid and weights are in [0, 1].
    static bool areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when sample coords exceed grid bounds or interpolation weights are outside [0, 1].
    static bool isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when corner indices lie within the grid, are ordered, and weights are in [0, 1].
    static bool isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when sample coords exceed grid bounds or interpolation weights are outside [0, 1].
    static bool isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when corner indices lie within grid bounds (weights unchecked).
    static bool areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when interpolation weights or corner indices would be clamped before sampling.
    static bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight without reject-reason diagnostics.
    static bool canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight; false on empty desc or hard OOB corners.
    static bool tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                              const ProbeSampleCoords& coords,
                                              ProbeSampleCoordsRejectReason& outReason);
    /// Early-out when sample-coord validation would reject — same ordering as `isValidProbeSampleCoords`.
    /// Early-out when sample-coord validation would reject; same ordering as `isValidProbeSampleCoords`.
    static bool wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight with reject-reason diagnostics.
    /// True when sample coords would be clamped into the valid grid range (B5.6 deepen).
    /// Sample-coord preflight; false when validation would reject (B5.6 deepen).
    static bool preflightProbeSampleCoords(const DDGIDesc& desc,
                                           ProbeSampleCoordsRejectReason* reason = nullptr);
    /// Early-out when building sample coords would reject (empty grid).
    static bool wouldSkipBuildProbeSampleCoords(const DDGIDesc& desc);
    /// Classify why sample-coord validation would reject; returns `None` on valid coords.
    static ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                                         const ProbeSampleCoords& coords);
    /// Early-out when sample-coord validation would reject — same ordering as `tryValidateProbeSampleCoords`.
    /// True when sample coords would be modified by clampProbeSampleCoords.
    /// Diagnose why sample-coord validation would reject; vacuously succeeds on valid coords.
    static bool tryValidateProbeSampleCoords(const DDGIDesc& desc,
                                             const ProbeSampleCoords& coords,
    /// True when sample coords would require clamp/normalize before sampling (B5.6 deepen pass).
    static bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight — ignores cache; soft-fails on clampable coords.
    static bool tryPreflightProbeSampleCoords(const DDGIDesc& desc,
    static bool canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Early-out when sample-coord preflight would be rejected — same ordering as `tryPreflightProbeSampleCoords`.
    static bool wouldSkipProbeSampleCoordPreflight(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Build sample coords from a world position and early-out when preflight would reject.
    static bool wouldSkipProbeSampleCoords(const DDGIDesc& desc, const fuse::math::Vec3& world_position);
    /// Early-out when grid-only sample-coord preflight would reject (B5.6 deepen pass).
    static bool shouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Classify sample-coord rejection — same ordering as `tryValidateProbeSampleCoords`.
    static ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                                         const ProbeSampleCoords& coords);
    /// Build + grid-validate sample coords without touching cache (B5.6 deepen pass).
    static bool preflightProbeSampleCoords(const DDGIDesc& desc,
                                           const fuse::math::Vec3& world_position,
                                           ProbeSampleCoords* out_coords = nullptr,
                                           ProbeSampleCoordsRejectReason* reason = nullptr);
    /// Last valid flat probe index; returns 0 when the grid has no probes.
    static u32 maxProbeIndex(const DDGIDesc& desc);
    /// True when corner indices and interpolation weights are within grid bounds.
    /// True when any trilinear corner lies on the probe-grid border shell.
    static bool isProbeSampleAtGridBorder(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when all eight trilinear corners are interior probes (full 2×2×2 neighbourhood).
    static bool hasFullTrilinearNeighbourhood(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Clamp sample coords to grid bounds and restore trilinear neighbour ordering.
    static void sanitizeProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// True when corner indices and blend weights are usable for trilinear lookup.
    /// Clamp sample coords; returns false and clears coords on empty grid.
    static bool areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Alias for `isValidProbeSampleCoords` — mirrors froxel `areSampleCoordsInBounds`.
    /// Clamp sample coords in place; returns false without modifying `coords` on an empty grid.
    static bool tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// Diagnose why sample coords fail validation; vacuously succeeds when valid.
                                             ProbeSampleCoordsRejectReason& outReason);
    /// Preflight guard before `buildProbeSampleCoords`; false on empty grid or invalid spacing.
    /// Preflight before `buildProbeSampleCoords`; false on empty grid or invalid spacing.
    static bool canBuildProbeSampleCoords(const DDGIDesc& desc);
    static bool isSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when corner indices and interpolation weights lie within grid bounds.
    /// Classify why sample-coord validation would reject — same ordering as `tryValidateProbeSampleCoords`.
    static ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                                         const ProbeSampleCoords& coords);
    /// Early-out when sample-coord validation would reject.
    static bool wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when corner indices or interpolation weights would be clamped before sampling.
    static bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Grid-only sample-coord preflight; false on empty grid or hard OOB corner indices.
    /// True when interpolation weights or corner indices would be clamped before sampling.
    /// Grid-only sample-coord preflight; false on empty grid or hard OOB corners.
    static bool tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                              const ProbeSampleCoords& coords,
    /// Grid-only sample-coord preflight without reject-reason diagnostics.
    static bool canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Early-out when sample-coord validation would reject — same ordering as `isValidProbeSampleCoords`.
    /// Preflight guard before trilinear sample-coord use — same ordering as `isValidProbeSampleCoords`.
    /// Diagnose why sample-coord preflight would reject; vacuously succeeds on valid coords.
    /// Early-out when sample-coord validation would reject — same ordering as `tryValidateProbeSampleCoords`.
    /// Classify why sample-coord validation would reject; returns `None` on valid coords.
    /// Early-out when sample-coord validation would reject — includes reject-reason diagnostics.
    static bool wouldSkipProbeSampleCoords(const DDGIDesc& desc,
    /// Early-out when sample-coord validation would be rejected — same ordering as `tryValidateProbeSampleCoords`.
    /// Sample-coord validation preflight with optional reject-reason output (B5.6 deepen).
    static bool preflightProbeSampleCoords(const DDGIDesc& desc,
                                           ProbeSampleCoordsRejectReason* reason = nullptr);
    /// Grid-only sample-coord preflight with reject-reason diagnostics.
    /// True when sample coords exceed grid bounds or interpolation weights are outside [0, 1].
    /// Classify why sample-coord validation would reject (B5.6 deepen).
    /// Early-out when sample-coord validation would be rejected (B5.6 deepen).
    /// Preflight sample-coord validation with optional reject-reason output (B5.6 deepen).
    /// Early-out when probe sample-coord preflight would reject (B5.6 deepen pass).
    static bool shouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Build sample coords only when grid build + validation succeed (B5.6 deepen pass).
    static bool buildProbeSampleCoordsIfReady(const DDGIDesc& desc,
                                              const fuse::math::Vec3& world_position,
                                              ProbeSampleCoords& out_coords);
    /// Build trilinear corner indices/weights from a world position; false when grid is empty.
    static bool buildProbeSampleCoords(const DDGIDesc& desc,
                                       const fuse::math::Vec3& world_position,
                                       ProbeSampleCoords& out_coords);
    /// Preflight guard before building sample coords from a world position.
    static bool canBuildProbeSampleCoords(const DDGIDesc& desc, const fuse::math::Vec3& world_position);
    /// Build sample coords with reject-reason diagnostics.
    /// Build sample coords with reject-reason diagnostics; false on empty grid.
    /// Build with guard preflight and reject-reason diagnostics.
    /// Build trilinear coords with reject-reason diagnostics (B5.6 deepen).
    /// Build sample coords with guard preflight and reject-reason diagnostics.
    static bool tryBuildProbeSampleCoords(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          ProbeSampleCoords& out_coords,
                                          ProbeSampleCoordsRejectReason& outReason);
    /// Build sample coords and validate indices/weights; false when build or validation fails.
                                          ProbeSampleCoords& out_coords);
    /// True when corner indices and trilinear weights are within grid bounds and [0, 1].
    static bool isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// True when any trilinear corner lies on the probe grid border shell.
    static bool sampleCoordsTouchBorder(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Build then clamp trilinear sample coordinates; false when grid is empty.
    static bool buildAndClampProbeSampleCoords(const DDGIDesc& desc,
    /// Clamp corner indices/weights then normalize ordering — no-op on empty grid.
    static void sanitizeProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// True when corner indices are ordered and weights lie in [0, 1].
    static bool isProbeSampleCoordsNormalized(const ProbeSampleCoords& coords);
    /// Validate sample coords with reject-reason diagnostics (B5.6 deepen).
    static bool tryValidateProbeSampleCoords(const DDGIDesc& desc,
                                             const ProbeSampleCoords& coords,
                                          ProbeSampleCoordRejectReason& outReason);
                                            ProbeSampleRejectReason& outReason);
    /// Build sample coords only when the grid is non-empty; returns false without modifying `out_coords` on empty grid (B5.6 deepen).
    static bool buildProbeSampleCoordsIfReady(const DDGIDesc& desc,
                                              const fuse::math::Vec3& world_position,
    /// Sample-coord build preflight with optional reject-reason output (B5.6 deepen).
    static bool preflightBuildProbeSampleCoords(const DDGIDesc& desc,
                                                ProbeSampleCoordsRejectReason* reason = nullptr);
    /// Build sample coords only when the grid is non-empty; false without modifying `out_coords` on reject.
    /// True when sample coords pass validation preflight (B5.6 deepen).
    static bool preflightProbeSampleCoords(const DDGIDesc& desc,
    /// Early-out when sample-coord validation would reject (B5.6 deepen).
    static bool shouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Fractional grid coordinates — origin cell centre is (0,0,0).
    static fuse::math::Vec3 worldToProbeGridCoord(const DDGIDesc& desc,
                                                  const fuse::math::Vec3& world_position);
    /// World→grid mapping with reject-reason diagnostics; false on empty grid or invalid spacing.
    static bool tryWorldToProbeGridCoord(const DDGIDesc& desc,
                                         const fuse::math::Vec3& world_position,
                                         fuse::math::Vec3& out_grid_coord,
                                         ProbeSampleCoordsRejectReason& outReason);
    /// True when corner indices or weights would be clamped before trilinear sampling.
    static bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Clamp fractional grid coordinates to the valid probe index range.
    static fuse::math::Vec3 clampWorldToProbeGridCoord(const DDGIDesc& desc,
                                                       const fuse::math::Vec3& grid_coord);
    static ProbeGridCoord clampProbeGridCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Top-left texel of the probe's octahedral irradiance tile in the atlas.
    static fuse::math::Vec2 probeIrradianceAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Absolute atlas texel for a world-space direction sample within a probe tile.
    static fuse::math::Vec2 probeIrradianceAtlasTexel(const DDGIDesc& desc,
                                                      const ProbeGridCoord& coord,
                                                      const fuse::math::Vec3& direction);
    /// Top-left texel of the probe's depth-variance tile in the atlas.
    static fuse::math::Vec2 probeDepthAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord);
};

/// Why a probe-cache index lookup preflight rejected the request (B5.6 deepen).
enum class CacheIndexRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRangeIndex,
    InvalidProbeIndex,
    UndersizedCache,
/// Why cache-index lookup preflight rejected (B5.6 deepen).
    NotSampleable,
};

/// Human-readable label for cache-index reject reasons (logging / tests).
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason);

/// Why a probe-update launch preflight rejected the request (B5.6 deepen).
enum class ProbeUpdateLaunchRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullIndices,
    ZeroCount,
    OutOfRangeIndex,
};

/// Human-readable label for probe-update launch reject reasons (logging / tests).
const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason);

/// CPU-side probe grid helpers — mirrors CUDA scheduling without GPU.
namespace ddgi_util {
u32 probeCount(const DDGIDesc& desc);
/// Returns 0 when the grid is empty.
u32 countBorderProbes(const DDGIDesc& desc);
/// Returns 0 when the grid is empty.
u32 countInteriorProbes(const DDGIDesc& desc);
/// Face/edge/corner breakdown; all fields zero on empty grid.
ProbeBorderCounts countProbesByBorderKind(const DDGIDesc& desc);
/// Count probes of a single border kind; returns 0 for `Invalid` or empty grids.
u32 countProbesOfBorderKind(const DDGIDesc& desc, ProbeBorderKind kind);
/// True when interior+border and face+edge+corner sums match `total`.
bool validateProbeBorderCounts(const ProbeBorderCounts& counts);
/// Recompute border-kind counts for `desc` and verify invariants.
bool validateProbeBorderCountsForGrid(const DDGIDesc& desc);
/// True when the probe grid can participate in spatial irradiance sampling.
bool canSampleProbeGrid(const DDGIDesc& desc);
/// Early-out when probe irradiance lookup should be skipped for an empty or non-sampleable grid.
bool shouldSkipProbeGrid(const DDGIDesc& desc);
/// Early-out when the probe grid cannot serve as an irradiance sample source (B5.6 deepen pass).
bool wouldSkipProbeGridSource(const DDGIDesc& desc);
/// Diagnose why probe-grid source preflight would reject; vacuously succeeds on sampleable grids.
bool tryValidateProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason& outReason);
/// Classify why probe-grid source preflight would reject — same ordering as `tryValidateProbeGridSource`.
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const DDGIDesc& desc);
/// Non-mutating probe-grid source preflight — returns true when sampling would proceed.
bool preflightProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason* reason = nullptr);
/// Probe-grid source preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason& outReason);
/// Early-out when probe cache lookup should be skipped (empty grid, null cache, or undersized storage).
bool shouldSkipProbeLookup(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count);
/// True when `cache` is allocated and sized for every probe in `desc`.
bool isProbeCacheAccessible(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count);
/// Preflight guard before coord-based probe trilinear sampling.
bool canSampleAtProbeCoords(const DDGIDesc& desc,
                            const ProbeSampleCoords& coords,
                            const IrradianceCacheEntry* cache,
                            u32 cache_count);
/// Classify why coord-based probe trilinear sampling would reject.
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count);
/// Early-out when coord-based probe trilinear sampling would reject.
bool shouldSkipTrilinearProbeSample(const DDGIDesc& desc,
                                    const ProbeSampleCoords& coords,
                                    const IrradianceCacheEntry* cache,
                                    u32 cache_count);
/// Diagnose why coord-based probe sample preflight would reject.
bool tryCanSampleAtProbeCoords(const DDGIDesc& desc,
                               u32 cache_count,
                               ProbeTrilinearSampleRejectReason& outReason);
/// Early-out when trilinear probe sampling would be rejected at fixed sample coords (B5.6 deepen pass).
bool wouldSkipTrilinearProbeSampleAtCoords(const DDGIDesc& desc,
/// Early-out when trilinear probe sampling would be rejected for a world position (B5.6 deepen pass).
bool wouldSkipTrilinearProbeSample(const DDGIDesc& desc,
                                   const fuse::math::Vec3& world_position,
/// Build sample coords and run trilinear preflight without sampling (B5.6 deepen pass).
bool preflightTrilinearProbeSample(const DDGIDesc& desc,
                                   ProbeSampleCoords* out_coords = nullptr,
                                   ProbeTrilinearSampleRejectReason* reason = nullptr);
/// Trilinear sample preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightTrilinearProbeSample(const DDGIDesc& desc,
                                      ProbeSampleCoords& out_coords,
/// Early-out when coord-based probe trilinear sampling would be rejected.
bool wouldSkipProbeTrilinearSample(const DDGIDesc& desc,
                                   const ProbeSampleCoords& coords,
                                   const IrradianceCacheEntry* cache,
                                   u32 cache_count);
/// Early-out when coord-based probe trilinear sampling would be rejected — includes diagnostics.
                                   u32 cache_count,
                                   ProbeTrilinearSampleRejectReason& outReason);
/// Early-out when coord-based probe sample preflight would reject.
bool wouldSkipCanSampleAtProbeCoords(const DDGIDesc& desc,
/// Classify why coord-based probe trilinear sampling preflight would reject (B5.6 deepen).
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
/// Early-out when world-position trilinear sampling would be rejected (B5.6 deepen).
bool shouldSkipTrilinearProbeSample(const DDGIDesc& desc,
/// Early-out when directional trilinear sampling would be rejected (B5.6 deepen).
bool shouldSkipTrilinearDirectionalProbeSample(const DDGIDesc& desc,
/// Early-out when coord-based probe trilinear sampling would be rejected — same ordering as `tryCanSampleAtProbeCoords`.
bool wouldSkipSampleAtProbeCoords(const DDGIDesc& desc,
/// Early-out when coord-based trilinear sampling would be rejected — same ordering as `tryCanSampleAtProbeCoords`.
bool wouldSkipTrilinearSampleAtCoords(const DDGIDesc& desc,
/// Early-out when world-space trilinear sampling would be rejected.
/// Early-out when coord-based probe trilinear sampling would be rejected — includes reject-reason diagnostics.
/// True when coord-based probe trilinear sampling preflight passes (B5.6 deepen).
/// Early-out when coord-based probe trilinear sampling would be rejected (B5.6 deepen).
/// Early-out when world-space probe trilinear sampling would be rejected.
/// Diagnose why probe-grid coord trilinear sampling preflight would reject (B5.6 deepen pass).
bool tryCanSampleAtProbeCoord(const DDGIDesc& desc,
                              u32 x,
                              u32 y,
                              u32 z,
/// Classify why coord-based probe trilinear sampling preflight would reject (B5.6 deepen pass).
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed (B5.6 deepen pass).
                                      ProbeTrilinearSampleRejectReason& reason);
/// Early-out when coord-based probe trilinear sampling would be rejected (B5.6 deepen pass).
/// True when coord-based probe trilinear sampling would proceed (B5.6 deepen pass).
bool trilinearProbeSampleReady(const DDGIDesc& desc,
/// Classify why coord-based probe sample preflight would reject — same ordering as `tryCanSampleAtProbeCoords`.
/// Early-out when coord-based trilinear sampling would be rejected.
/// Non-mutating coord-based trilinear sample preflight — returns true when lookup would proceed.
/// Build sample coords + cache preflight — returns true when trilinear lookup would proceed.
/// Diagnose cache-index preflight for every scheduled probe index; vacuously succeeds when all valid.
bool tryValidateScheduledCacheIndices(const DDGIDesc& desc,
                                      const u32* probe_indices,
                                      u32 probe_count,
                                      CacheIndexRejectReason& outReason);
/// Diagnose cache-index preflight for scheduled indices including null-cache rejection.
/// Non-mutating cache-index preflight — returns true when lookup would proceed (no cache pointer).
bool preflightCacheIndexLookup(const DDGIDesc& desc,
                               u32 probe_index,
                               CacheIndexRejectReason* reason = nullptr);
/// Probes that would be scheduled after capacity/probe-count caps (B5.6 deepen pass).
u32 effectiveScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices);
/// Classify why coord-based probe trilinear preflight would reject.
ProbeTrilinearSampleRejectReason classifyTrilinearProbeSampleReject(const DDGIDesc& desc,
/// Classify why world-position trilinear preflight would reject.
/// Non-mutating trilinear sample preflight — returns true when lookup would proceed.
bool preflightTrilinearProbeIrradiance(const DDGIDesc& desc,
/// Early-out when trilinear sample preflight would reject.
bool wouldSkipTrilinearProbeIrradiance(const DDGIDesc& desc,
/// Classify why coord-based trilinear sampling would reject — same ordering as `tryCanSampleAtProbeCoords`.
/// Classify why world-position trilinear sampling would reject — builds sample coords first.
/// Non-mutating trilinear sample preflight at built sample coords.
bool preflightTrilinearProbeSampleAtCoords(const DDGIDesc& desc,
/// Non-mutating trilinear sample preflight from a world position.
/// Early-out when world-position trilinear sampling would be rejected.
/// Read irradiance at a probe index with guard preflight; returns false when lookup would be rejected.
bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              u32 probe_index,
                              fuse::math::Vec3& out_irradiance);
/// Read irradiance with cache-index reject-reason diagnostics.
bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              u32 probe_index,
                              fuse::math::Vec3& out_irradiance,
                              CacheIndexRejectReason& outReason);
                              fuse::math::Vec3& out_irradiance);
/// Read irradiance with guard preflight and cache-index reject-reason diagnostics.
/// Read irradiance with guard preflight and reject-reason diagnostics.
/// Read irradiance with guard preflight and reject-reason diagnostics (B5.6 deepen).
/// Read irradiance at a probe index with guard preflight and reject-reason diagnostics.
/// Read irradiance with reject-reason diagnostics (B5.6 deepen).
bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              u32 probe_index,
/// True when a cache lookup at `probe_index` would clamp into the valid probe range.
bool wouldClampCacheIndex(u32 probe_index, const DDGIDesc& desc);
                              fuse::math::Vec3& out_irradiance,
                              CacheIndexRejectReason& outReason);
/// Early-out when cache-index irradiance read would be rejected — same ordering as `tryReadIrradianceAtIndex`.
bool wouldSkipReadIrradianceAtIndex(const DDGIDesc& desc,
                                    u32 probe_index);
/// Read irradiance only when cache-index preflight passes; false without writing on reject (B5.6 deepen).
bool readIrradianceAtIndexIfReady(const DDGIDesc& desc,
                                  fuse::math::Vec3& out_irradiance);
/// Read irradiance at a probe grid coord with guard preflight.
bool tryReadIrradianceAtCoord(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              const ProbeGridCoord& coord,
/// Read irradiance at a probe grid coord with guard preflight and reject-reason diagnostics.
/// Read irradiance at probe-grid coordinates with guard preflight and reject-reason diagnostics.
                              u32 x,
                              u32 y,
                              u32 z,
/// Minimum irradiance-cache entries for trilinear sampling; 0 when the grid is not sampleable.
u32 requiredCacheCount(const DDGIDesc& desc);
/// True when `cache_count` covers every probe in `desc`.
bool isCacheSizedForGrid(const DDGIDesc& desc, u32 cache_count);
/// Probe-cache shortfall vs `requiredCacheCount`; 0 when sized or the grid is not sampleable.
u32 cacheEntriesMissing(const DDGIDesc& desc, u32 cache_count);
/// Combined probe-index + cache-length guard for cache lookups.
bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// True when cache lookup would be rejected by `isCacheIndexValid`.
bool isCacheIndexOutOfRange(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Classify why cache-index preflight would reject — same ordering as `tryValidateCacheIndex`.
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Classify cache-index preflight including null-cache rejection.
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                               const IrradianceCacheEntry* cache,
                                               u32 probe_index,
                                               u32 cache_count);
/// Combined probe-index + cache-pointer guard for cache lookups.
bool isCacheIndexValid(const DDGIDesc& desc,
/// Preflight guard before coord-based cache lookup; false on empty grid or undersized cache.
bool canLookupAtCoord(const DDGIDesc& desc, const ProbeGridCoord& coord, u32 cache_count);
/// Diagnose why coord-based cache lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const DDGIDesc& desc,
                         const ProbeGridCoord& coord,
                         u32 cache_count,
                         CacheIndexRejectReason& outReason);
/// True when a cache lookup at `probe_index` would clamp into the valid probe range.
bool wouldClampCacheIndexLookup(u32 probe_index, const DDGIDesc& desc);
/// Diagnose why cache-index preflight would reject; vacuously succeeds on valid indices.
bool tryValidateCacheIndex(const DDGIDesc& desc,
/// Diagnose cache-index preflight including null-cache rejection.
/// Diagnose cache-index preflight after clamping OOB probe indices (B5.6 deepen pass).
bool tryValidateCacheIndexAfterClamp(const DDGIDesc& desc,
bool tryValidateCacheIndex(const DDGIDesc& desc,
                           const IrradianceCacheEntry* cache,
                           u32 probe_index,
                           u32 cache_count,
                           CacheIndexRejectReason& outReason);
/// Preflight guard before probe-index cache lookup; false on empty grid, null cache, or OOB index.
bool canLookupAtProbeIndex(const DDGIDesc& desc,
                           u32 cache_count);
/// Diagnose why probe-index lookup preflight would reject.
bool tryCanLookupAtProbeIndex(const DDGIDesc& desc,
/// True when a lookup at `probe_index` would clamp into the valid probe range.
/// Cache-index preflight with optional reject-reason output (B5.6 deepen).
bool preflightCacheIndexLookup(const DDGIDesc& desc,
/// Classify why cache-index preflight would reject (B5.6 deepen).
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Classify cache-index preflight including null-cache rejection (B5.6 deepen).
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
/// True when cache-index lookup preflight would succeed (B5.6 deepen).
bool cacheIndexReady(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// True when cache-index lookup preflight would succeed — includes null-cache check (B5.6 deepen).
bool cacheIndexReady(const DDGIDesc& desc,
/// Early-out when cache-index lookup would be rejected — same ordering as `tryValidateCacheIndex`.
bool wouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Early-out when cache-index lookup would be rejected — includes null-cache check.
bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
                               u32 probe_index,
                               u32 cache_count,
                               CacheIndexRejectReason* reason = nullptr);
/// Cache-index preflight with optional reject-reason output — includes null-cache check (B5.6 deepen).
                               const IrradianceCacheEntry* cache,
/// True when `probe_index` exceeds the valid probe range and would be clamped (B5.6 deepen).
bool wouldClampCacheIndexLookup(u32 probe_index, const DDGIDesc& desc);
bool preflightCacheIndex(const DDGIDesc& desc,
/// Early-out when irradiance cache read would be rejected — same ordering as `tryReadIrradianceAtIndex`.
bool wouldSkipReadIrradianceAtIndex(const DDGIDesc& desc,
                                    u32 probe_index);
/// Classify why cache-index preflight would reject; returns `None` on valid indices.
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
/// Classify why cache-index preflight would reject (B5.6 deepen).
/// Classify cache-index preflight including null-cache rejection (B5.6 deepen).
/// True when a cache lookup at `probe_index` would clamp into the valid probe range.
bool wouldClampCacheIndexLookup(const DDGIDesc& desc, u32 probe_index);
/// Early-out when cache-index lookup would be rejected — same ordering as `tryValidateCacheIndex`.
bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
/// Early-out when cache-index lookup would be rejected — includes null-cache check.
/// Early-out when cache lookup would fail even after clamping probe index (B5.6 deepen pass).
bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc,
/// Early-out when coord-based cache lookup would be rejected (B5.6 deepen pass).
bool wouldSkipCacheIndexLookupAtCoord(const DDGIDesc& desc,
                                      const ProbeGridCoord& coord,
/// Classify why cache-index preflight would reject — same ordering as `tryValidateCacheIndex`.
/// Non-mutating cache-index preflight — returns true when lookup would proceed.
bool preflightCacheIndexLookup(const DDGIDesc& desc,
/// Cache-index preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightCacheIndexLookup(const DDGIDesc& desc,
                               u32 cache_count);
/// Classify why cache-index preflight would reject at probe-grid coordinates (B5.6 deepen pass).
CacheIndexRejectReason classifyCacheIndexRejectAtCoord(const DDGIDesc& desc,
                                                       u32 x,
                                                       u32 y,
                                                       u32 z,
                                  CacheIndexRejectReason& reason);
/// Early-out when cache-index lookup would be rejected — includes null-cache check (B5.6 deepen pass).
bool shouldSkipCacheIndexLookup(const DDGIDesc& desc,
/// True when probe-grid coordinates exceed grid bounds (would be clamped before lookup).
bool wouldClampCacheIndexCoord(const DDGIDesc& desc, u32 x, u32 y, u32 z);
                                  u32 probe_index,
                                  u32 cache_count,
/// Early-out when cache-index lookup would be rejected (B5.6 deepen pass).
/// True when cache-index lookup would proceed (B5.6 deepen pass).
bool cacheIndexLookupReady(const DDGIDesc& desc,
/// True when `probe_index` exceeds the valid probe range on a non-empty grid.
bool wouldClampProbeIndexForLookup(u32 probe_index, const DDGIDesc& desc);
/// Minimum irradiance cache entries required for full-grid sampling; 0 on empty grid.
/// Shortfall below `requiredCacheCount`; 0 when cache is sized or grid is empty.
u32 cacheDeficitForGrid(const DDGIDesc& desc, u32 cache_count);
/// Diagnose grid-level guards that would block sampling (ignores cache sizing).
ProbeSampleSkipReason classifyProbeGridSkip(const DDGIDesc& desc);
/// Diagnose the first grid/cache guard that would block sampling.
ProbeSampleSkipReason classifyProbeSampleSkip(const DDGIDesc& desc, u32 cache_count);
/// Diagnose lookup guards including null cache pointer checks.
ProbeSampleSkipReason classifyProbeSampleLookup(const DDGIDesc& desc,
/// Required CPU cache entry count for the probe volume; 0 on empty grid.
u32 expectedCacheCount(const DDGIDesc& desc);
/// True when `cache_count` exactly matches `expectedCacheCount`.
bool cacheMatchesGrid(const DDGIDesc& desc, u32 cache_count);
/// True when `cache_index` is a valid offset into a cache buffer of `cache_count` entries.
bool isCacheIndexInRange(u32 cache_index, u32 cache_count);
/// Clamp a flat cache index to [0, min(cache_count, probeCount) - 1]; returns 0 when empty.
u32 clampCacheIndex(u32 cache_index, const DDGIDesc& desc, u32 cache_count);
/// Guarded irradiance read; returns zero when the index is out of range or `cache` is null.
fuse::math::Vec3 sampleIrradianceAtCacheIndex(const IrradianceCacheEntry* cache,
                                              u32 cache_index);
/// Minimum irradiance-cache length required for full-grid trilinear sampling.
/// Minimum cache entries required for full-grid trilinear sampling (equals `probeCount`).
/// True when `probe_index` is a valid offset into a cache of `cache_count` entries.
bool isProbeIndexCacheAccessible(u32 probe_index, u32 cache_count);
/// True when `probe_index` exceeds the cache range (would be clamped or rejected).
bool isProbeIndexCacheOutOfRange(u32 probe_index, u32 cache_count);
/// Clamp a flat probe index to [0, cache_count - 1]; returns 0 when the cache is empty.
u32 clampProbeIndexForCache(u32 probe_index, u32 cache_count);
/// Preflight guard before index-based cache lookup; false on empty grid or undersized cache.
bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 cache_count, u32 probe_index);
/// Diagnose why cache lookup preflight would reject.
bool tryCanLookupCacheAtIndex(const DDGIDesc& desc,
                              ProbeCacheLookupRejectReason& outReason);
/// Flat probe index for cache access after coord clamp; returns 0 on empty grid.
u32 cacheIndexFromClampedCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
/// Read irradiance at clamped probe index; returns false when preflight rejects.
bool trySampleCacheAtIndex(const DDGIDesc& desc,
                           fuse::math::Vec3& outIrradiance);
/// Read irradiance at probe grid coord with guard preflight.
bool trySampleCacheAtCoord(const DDGIDesc& desc,
/// True when `probe_index` is out of grid range or exceeds `cache_count`.
/// Diagnose why cache-index preflight would reject; vacuously succeeds on valid lookups.
bool tryIsCacheIndexValid(const DDGIDesc& desc,
/// Diagnose why a cache-index guard would reject; vacuously succeeds when valid.
                          CacheIndexRejectReason& outReason);
    /// Preflight guard before index-based cache lookup; false when grid or cache is inaccessible.
    bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 cache_count);
    /// Diagnose why cache lookup preflight would reject; vacuously succeeds on accessible grids.
    bool tryCanLookupCacheAtProbeIndex(const DDGIDesc& desc,
    /// Cache-index guard after `clampProbeIndex` — safe for OOB scheduling indices.
    bool isCacheIndexValidForClampedIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Diagnose why cache index lookup would reject (B5.6 deepen).
/// Diagnose why cache sizing would reject sampling (B5.6 deepen).
bool tryValidateCacheSizedForGrid(const DDGIDesc& desc,
/// Diagnose extended cache lookup preflight (sampleable grid, non-zero cache, valid index).
/// Preflight guard before coord-based probe irradiance sampling; false on inaccessible grid or invalid coords.
bool canSampleAtProbeCoords(const DDGIDesc& desc,
                            const ProbeSampleCoords& coords,
                            u32 cache_count);
/// Diagnose why coord-based sample preflight would reject.
bool tryCanSampleAtProbeCoords(const DDGIDesc& desc,
                               ProbeSampleCoordRejectReason& outReason);
/// Preflight guard before cache-index lookup; false on empty grid or undersized cache.
bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Diagnose why cache-index lookup preflight would reject.
                              CacheLookupRejectReason& outReason);
/// Early-out when probe irradiance sampling should be skipped.
bool shouldSkipProbeSample(const DDGIDesc& desc, u32 cache_count);
/// Early-out when probe update launch should be skipped for an empty grid.
bool shouldSkipProbeUpdate(const DDGIDesc& desc);
/// Preflight guard before coord-based probe sampling; false on inaccessible grid or invalid coords.
/// Diagnose why coord-based probe sample preflight would reject.
                               ProbeSampleRejectReason& outReason);
                               ProbeSampleCoordsRejectReason& outReason);
/// Preflight guard before cache lookup at a probe grid coordinate.
bool canLookupCacheAtCoord(const DDGIDesc& desc, const ProbeGridCoord& coord, u32 cache_count);
/// Diagnose why coord-based cache lookup preflight would reject.
bool tryCanLookupCacheAtCoord(const DDGIDesc& desc,
/// Preflight guard before spatial trilinear sampling at probe sample coords.
bool canSampleAtProbeCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords, u32 cache_count);
/// Diagnose why spatial sample preflight would reject.
                               ProbeSpatialSampleRejectReason& outReason);
/// Diagnose why cache access preflight would reject; checks null cache before index validation.
bool tryValidateCacheAccess(const DDGIDesc& desc,
/// Diagnose why cache lookup preflight would reject; checks null cache before index bounds.
bool tryValidateCacheLookup(const DDGIDesc& desc,
/// Early-out when cache-index validation would be rejected.
bool wouldSkipCacheIndexValidation(const DDGIDesc& desc,
/// Diagnose why cache-index preflight would reject, including null-cache guard.
/// Classify cache-index reject including null-cache guard.
/// Early-out when cache-index preflight would reject.
bool wouldSkipCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Early-out when cache-index preflight would reject, including null-cache guard.
bool wouldSkipCacheIndex(const DDGIDesc& desc,
/// Classify why coord-based probe trilinear sampling preflight would reject.
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(
    const DDGIDesc& desc,
/// Early-out when coord-based probe trilinear sampling preflight would reject.
bool wouldSkipTrilinearProbeSample(const DDGIDesc& desc,
/// Cache-index preflight with null-cache detection; vacuously succeeds on valid indices.
/// True when `probe_index` exceeds the grid or `cache_count` is undersized.
bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Diagnose why cache-index preflight would reject, including null-cache detection.
/// Early-out when cache-index lookup would be rejected — same ordering as `isCacheIndexValid`.
/// Early-out when cache-index lookup would be rejected, including null-cache detection.
/// Combined probe-index + cache pointer guard for cache lookups.
bool isCacheIndexValid(const DDGIDesc& desc,
/// Diagnose why cache-index preflight would reject, including null-cache check.
/// Diagnose cache-index + null-cache preflight; vacuously succeeds on valid lookups.
bool tryValidateCacheIndexLookup(const DDGIDesc& desc,
bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index);
/// Cache-index preflight including null-cache diagnostics.
bool wouldClampCacheLookupIndex(u32 probe_index, const DDGIDesc& desc);
/// World position with guard preflight; false on empty grid or OOB probe index.
bool tryProbeWorldPosition(const DDGIDesc& desc,
                           fuse::math::Vec3& out_position,
/// Combined probe-index + cache pointer/length guard for cache lookups.
/// Diagnose why cache-index preflight would reject; includes null-cache guard.
/// Full cache lookup preflight including null-cache rejection.
/// True when a lookup at `probe_index` would clamp into the valid probe range.
bool wouldClampProbeIndex(u32 probe_index, const DDGIDesc& desc);
/// Early-out when cache lookup should be skipped (empty grid, null cache, or undersized storage).
bool shouldSkipCacheLookup(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count);
/// Diagnose why cache-index preflight would reject; checks `cache` for null (B5.6 deepen).
bool tryValidateCacheIndex(const DDGIDesc& desc,
/// Combined probe-index + cache guard for cache lookups (B5.6 deepen).
/// Cache-index preflight including null-cache guard; vacuously succeeds on valid lookups.
/// Early-out when cache-index lookup would be rejected — mirrors `wouldSkipCacheIndexLookup`.
bool shouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
bool shouldSkipCacheIndexLookup(const DDGIDesc& desc,
/// Early-out when cache-index lookup would be rejected — same ordering as `wouldSkipCacheIndexLookup` (B5.6 deepen).
/// Early-out when cache-index lookup would be rejected — includes null-cache check (B5.6 deepen).
/// Cache-index preflight; false when validation would reject (B5.6 deepen).
/// Cache-index preflight including null-cache check (B5.6 deepen).
/// Early-out when guarded cache read would be rejected — same ordering as `tryReadIrradianceAtIndex`.
bool wouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Early-out when cache-index lookup would be rejected — includes reject-reason diagnostics.
/// Early-out when cache-index lookup would be rejected — null-cache check with reject-reason diagnostics.
/// Early-out with reject-reason diagnostics (B5.6 deepen).
/// Early-out with reject-reason diagnostics — includes null-cache check (B5.6 deepen).
/// True when cache-index lookup preflight passes (B5.6 deepen).
bool cacheIndexLookupReady(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// True when cache-index lookup preflight passes — includes null-cache check (B5.6 deepen).
bool cacheIndexLookupReady(const DDGIDesc& desc,
/// True when cache-index preflight passes (B5.6 deepen).
/// True when cache-index preflight passes — includes null-cache check (B5.6 deepen).
/// Sample-request guard — grid ready and cache sized for trilinear lookup (empty normals resolve at sample time).
    /// True when `probe_index` is out of range for the grid or exceeds `cache_count`.
    bool isCacheIndexOutOfRange(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
    /// Clamp `probe_index` for cache access; returns false and zeroes `out_index` on an empty grid.
    bool tryClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count, u32& out_index);
bool isValidSampleRequest(const DDGIDesc& desc,
                          const DDGISampleRequest& request,
                          u32 cache_count);
/// True when `probe_index` maps to a valid grid cell and lies within `cache_count`.
bool canAccessCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Diagnose why a sample request preflight would reject.
bool tryIsValidSampleRequest(const DDGIDesc& desc,
                             const DDGISampleRequest& request,
                             u32 cache_count,
                             CacheLookupRejectReason& outReason);
/// Diagnose why sample-request preflight would reject; vacuously succeeds on valid requests.
bool tryValidateSampleRequest(const DDGIDesc& desc,
                              SampleRequestRejectReason& outReason);
/// Early-out when sample-request preflight would reject — same ordering as `isValidSampleRequest`.
bool wouldSkipSampleRequest(const DDGIDesc& desc,
                            u32 cache_count);
/// Diagnose why sample-request preflight would reject (B5.6 deepen).
/// Early-out when DDGI irradiance sampling would be rejected (B5.6 deepen).
bool wouldSkipDdgiSample(const DDGIDesc& desc, const DDGISampleRequest& request, u32 cache_count);
fuse::math::Vec3 probeWorldPosition(const DDGIDesc& desc, u32 probe_index);
/// World position after `clampProbeIndex` — safe for OOB scheduling indices.
fuse::math::Vec3 probeWorldPositionClamped(const DDGIDesc& desc, u32 probe_index);
u32 irradianceAtlasWidth(const DDGIDesc& desc);
u32 irradianceAtlasHeight(const DDGIDesc& desc);
u32 depthAtlasWidth(const DDGIDesc& desc);
u32 depthAtlasHeight(const DDGIDesc& desc);
/// Preflight guard before probe scheduling; false on null output or zero capacity.
bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Diagnose why probe scheduling preflight would reject.
bool tryScheduleProbeUpdates(u32 probe_count,
                             u32 max_indices,
                             const u32* out_indices,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason);
/// Why probe round-robin scheduling preflight rejected the request (B5.6 deepen).
enum class ProbeScheduleRejectReason : u8 {
    None = 0,
    ZeroProbeCount,
    NullIndicesBuffer,
    NullCountOut,
    ZeroMaxIndices,
    ZeroProbesPerFrame,
};

/// Human-readable label for probe-schedule reject reasons (logging / tests).
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason);

/// Preflight guard before probe scheduling; false on null outputs or zero capacity.
/// Diagnose why probe scheduling preflight would reject; vacuously succeeds when schedulable.
bool tryCanScheduleProbeUpdates(u32 probe_count,
/// Early-out when probe scheduling would be rejected — same ordering as `canScheduleProbeUpdates`.
bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
bool tryValidateProbeSchedule(u32 probe_count,
                              u32 probes_per_frame,
                              const u32* out_count,
/// Preflight guard before probe round-robin scheduling.
bool canScheduleProbeUpdates(u32 probe_count,
                             const u32* out_count);
bool wouldSkipProbeSchedule(u32 probe_count,
void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count);
/// Classify why probe scheduling preflight would reject — same ordering as `tryCanScheduleProbeUpdates`.
ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count);
/// Preflight guard before probe-update scheduling; false on null outputs or zero capacity.
/// Preflight guard before probe round-robin scheduling.
bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Early-out when probe scheduling would be rejected — same ordering as `canScheduleProbeUpdates`.
bool wouldSkipProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
                            ProbeScheduleRejectReason* reason = nullptr);
bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Early-out when probe scheduling would be rejected — mirrors `wouldSkipProbeSchedule`.
bool shouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Early-out when probe scheduling would be rejected — same ordering as `wouldSkipProbeSchedule` (B5.6 deepen).
/// Probe-schedule preflight; false when scheduling would be rejected (B5.6 deepen).
bool preflightProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
                            ProbeScheduleRejectReason* reason = nullptr);
/// Early-out when probe scheduling would be rejected — includes reject-reason diagnostics.
bool wouldSkipProbeSchedule(u32 probe_count,
/// Early-out with reject-reason diagnostics (B5.6 deepen).
                            ProbeScheduleRejectReason& outReason);
/// Diagnose why probe scheduling preflight would reject; vacuously succeeds when schedulable.
bool tryCanScheduleProbeUpdates(u32 probe_count,
                               u32 max_indices,
                               const u32* out_indices,
                               u32* out_count,
                               ProbeScheduleRejectReason& outReason);
/// Preflight guard before probe-update scheduling including per-frame rate; false when rate is zero.
bool canScheduleProbeUpdatesAtRate(u32 probe_count,
                                   u32 probes_per_frame,
                                   u32* out_count);
/// Early-out when rate-aware probe scheduling would be rejected.
bool wouldSkipProbeScheduleAtRate(u32 probe_count,
/// Diagnose why rate-aware probe scheduling preflight would reject.
bool tryCanScheduleProbeUpdatesAtRate(u32 probe_count,
/// Classify why rate-aware probe scheduling would reject — same ordering as `tryCanScheduleProbeUpdatesAtRate`.
ProbeScheduleRejectReason classifyProbeScheduleRejectAtRate(u32 probe_count,
/// Non-mutating rate-aware schedule preflight — returns true when scheduling would proceed.
bool preflightProbeScheduleAtRate(u32 probe_count,
                                  ProbeScheduleRejectReason* reason = nullptr);
/// Rate-aware schedule preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightProbeScheduleAtRate(u32 probe_count,
/// Probe scheduling preflight with optional reject-reason output (B5.6 deepen).
bool preflightProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
/// Schedule probe updates with reject-reason diagnostics; false when preflight rejects.
bool canScheduleProbeUpdates(u32 probe_count,
bool wouldSkipProbeSchedule(u32 probe_count,
                             const u32* out_count);
/// Early-out when probe scheduling would be rejected.
/// Schedule probes with reject-reason diagnostics; false when preflight rejects.
/// Preflight guard before probe scheduling; false on null buffers or zero capacity.
/// Classify why probe scheduling preflight would reject — same ordering as `tryCanScheduleProbeUpdates`.
ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
/// Early-out when probe scheduling preflight would reject.
/// Schedule with reject-reason diagnostics; leaves buffers unchanged on reject.
/// Preflight guard before probe scheduling; false on null outputs or zero capacity.
bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, const u32* out_count);
bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, const u32* out_count);
/// Early-out when probe-update scheduling would be rejected.
/// Diagnose why probe-update scheduling preflight would reject; vacuously succeeds when schedulable.
/// Preflight guard before probe scheduling; false on null/zero output buffers or zero probe count.
                             u32* out_indices,
/// Preflight guard before probe scheduling; false on null outputs or zero `max_indices`.
/// Preflight guard before probe scheduling; false on zero probe count or null outputs.
                                const u32* out_count,
/// Preflight guard before probe-update scheduling; false on null output or zero capacity.
/// Early-out when probe-update scheduling would be rejected — same ordering as `canScheduleProbeUpdates`.
/// Schedule probe updates with reject-reason diagnostics; vacuously succeeds on valid no-ops.
bool tryScheduleProbeUpdates(u32 frame_index,
                             u32 probe_count,
                             u32 probes_per_frame,
                             u32* out_indices,
                             u32 max_indices,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason);
/// Rate-aware schedule with reject-reason diagnostics; false when preflight rejects (B5.6 deepen pass).
bool tryScheduleProbeUpdatesAtRate(u32 frame_index,
                                   u32 probe_count,
                                   u32 probes_per_frame,
                                   u32* out_indices,
                                   u32 max_indices,
                                   u32* out_count,
                                   ProbeScheduleRejectReason& outReason);
/// Classify why probe scheduling would be rejected — same ordering as `tryCanScheduleProbeUpdates`.
ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      const u32* out_indices,
                                                      u32* out_count);
/// Classify why rate-aware probe scheduling would be rejected.
/// Classify why rate-aware probe scheduling would be rejected — same ordering as `tryCanScheduleProbeUpdatesAtRate`.
ProbeScheduleRejectReason classifyProbeScheduleRejectAtRate(u32 probe_count,
                                                            u32 probes_per_frame,
                                                            u32 max_indices,
                                                            const u32* out_indices,
                                                            u32* out_count);
/// Non-mutating rate-aware schedule preflight — returns true when scheduling would proceed.
bool preflightProbeScheduleAtRate(u32 probe_count,
                                  u32 probes_per_frame,
                                  u32 max_indices,
                                  const u32* out_indices,
                                  u32* out_count,
                                  ProbeScheduleRejectReason* reason = nullptr);
/// Schedule probe updates with rate-aware reject-reason diagnostics; false when preflight rejects.
bool tryScheduleProbeUpdatesAtRate(u32 frame_index,
                                   u32 probe_count,
                                   u32* out_indices,
                                   ProbeScheduleRejectReason& outReason);
/// Non-mutating schedule preflight — returns true when scheduling would proceed.
bool preflightProbeSchedule(u32 probe_count,
                            ProbeScheduleRejectReason* reason = nullptr);
/// Schedule preflight with mandatory reject-reason output (B5.6 deepen pass).
bool tryPreflightProbeSchedule(u32 probe_count,
                               u32 max_indices,
                               const u32* out_indices,
                               u32* out_count,
                               ProbeScheduleRejectReason& reason);
/// Early-out when probe scheduling would be rejected (B5.6 deepen pass).
bool shouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
bool shouldSkipProbeSchedule(u32 probe_count,
                             u32* out_count);
/// Non-mutating rate-aware schedule preflight — returns true when scheduling would proceed.
bool preflightProbeScheduleAtRate(u32 probe_count,
                                  u32 probes_per_frame,
                                  ProbeScheduleRejectReason* reason = nullptr);
/// Schedule probe updates with rate-aware reject-reason diagnostics; false when preflight rejects.
bool tryScheduleProbeUpdatesAtRate(u32 frame_index,
                                   u32 probe_count,
                                   u32* out_indices,
                                   ProbeScheduleRejectReason& outReason);
/// True when output capacity would cap scheduled probes below `probes_per_frame`.
bool wouldClampScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices);
/// Early-out when probe scheduling would be rejected — same ordering as `tryScheduleProbeUpdates`.
bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Early-out when probe scheduling would be rejected — same ordering as `canScheduleProbeUpdates`.
bool wouldSkipProbeSchedule(u32 probe_count, u32* out_indices, u32 max_indices, u32* out_count);
/// Preflight guard before probe scheduling; false on null output buffers.
bool canScheduleProbeUpdates(u32 probe_count,
bool wouldSkipProbeSchedule(u32 probe_count,
/// Probes actually scheduled given capacity — `min(probes_per_frame, probe_count, max_indices)`.
u32 effectiveScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices);
/// True when scheduling would write fewer probes than requested due to capacity or grid size.
/// Full schedule preflight including probes-per-frame; false when preflight rejects.
bool tryCanScheduleProbeUpdates(u32 frame_index,
/// Probe scheduling preflight with optional reject-reason output (B5.6 deepen).
/// Validate scheduled probe indices against `desc`; false when any index is out of range.
bool tryValidateScheduledProbeIndices(const DDGIDesc& desc,
                                      const u32* scheduled_indices,
                                      u32 scheduled_count,
                                      ProbeUpdateLaunchRejectReason& outReason);
/// Classify why probe-update scheduling preflight would reject (B5.6 deepen).
/// True when probe scheduling preflight passes (B5.6 deepen).
bool probeScheduleReady(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count);
/// Classify why probe scheduling preflight would reject (B5.6 deepen).
/// Preflight probe scheduling with optional reject-reason output (B5.6 deepen).
bool preflightScheduleProbeUpdates(u32 probe_count,
/// Validate scheduled probe indices before host/CUDA launch (B5.6 deepen).
bool validateScheduledProbeIndices(const DDGIDesc& desc,
                                   const u32* probe_indices,
fuse::math::Vec3 blendIrradiance(const fuse::math::Vec3& previous,
                                 const fuse::math::Vec3& incoming,
                                 f32 hysteresis);
fuse::math::Vec3 lerpIrradiance(const fuse::math::Vec3& a, const fuse::math::Vec3& b, f32 t);
/// Bilinear irradiance lerp within a probe's octahedral tile (CPU stub).
/// `samples` must point to four irradiance values in [00, 10, 01, 11] order.
fuse::math::Vec3 bilinearTileIrradiance(const fuse::math::Vec3* samples, f32 u, f32 v);
fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count);
/// Trilinear sample with guard preflight; returns false when lookup would be rejected.
/// Guarded trilinear sample — returns false when grid/cache/sample preflight fails.
bool tryTrilinearProbeIrradiance(const DDGIDesc& desc,
                                 const fuse::math::Vec3& world_position,
                                 const IrradianceCacheEntry* cache,
                                 u32 cache_count,
                                 fuse::math::Vec3& out_irradiance);
/// Trilinear sample with guard preflight and reject-reason diagnostics.
bool tryTrilinearProbeIrradiance(const DDGIDesc& desc,
                                 const fuse::math::Vec3& world_position,
                                 const IrradianceCacheEntry* cache,
                                 u32 cache_count,
                                 fuse::math::Vec3& out_irradiance,
                                 ProbeTrilinearSampleRejectReason& outReason);
                                 ProbeSpatialSampleRejectReason& outReason);
/// Early-out when trilinear probe sampling would be rejected.
bool wouldSkipTrilinearProbeIrradiance(const DDGIDesc& desc,
                                       const fuse::math::Vec3& world_position,
                                       const IrradianceCacheEntry* cache,
                                       u32 cache_count);
/// Trilinear sample only when preflight passes; false without writing on reject (B5.6 deepen).
bool trilinearProbeIrradianceIfReady(const DDGIDesc& desc,
                                     u32 cache_count,
                                     fuse::math::Vec3& out_irradiance);
/// Directional octahedral bilinear sample within one probe cache entry (CPU stub).
fuse::math::Vec3 sampleDirectionalIrradianceAtProbe(const IrradianceCacheEntry& entry,
                                                  const fuse::math::Vec3& direction,
                                                  u32 irradiance_res);
/// Spatial trilinear probe blend with per-probe octahedral direction sampling.
fuse::math::Vec3 trilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                                     const fuse::math::Vec3& world_position,
                                                     const fuse::math::Vec3& direction,
                                                     const IrradianceCacheEntry* cache,
                                                     u32 cache_count);
/// Directional trilinear sample with guard preflight; returns false when lookup would be rejected.
/// Guarded directional trilinear sample — returns false when grid/cache/sample preflight fails.
bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance);
/// Directional trilinear sample with guard preflight and reject-reason diagnostics.
bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance,
                                            ProbeTrilinearSampleRejectReason& outReason);
                                            ProbeSpatialSampleRejectReason& outReason);
/// Early-out when directional trilinear probe sampling would be rejected.
bool wouldSkipTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                                const fuse::math::Vec3& world_position,
                                                const IrradianceCacheEntry* cache,
                                                u32 cache_count);
u32 nearestProbeIndex(const DDGIDesc& desc, const fuse::math::Vec3& world_position);
} // namespace ddgi_util

DdgiInfo ddgi_info();

/// DDGI probe volume scaffold — allocates atlas textures via ResourceManager.
class DDGI {
public:
    DDGI() = default;

    bool init(const DDGIDesc& desc, ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_ready; }
    const DDGIDesc& desc() const { return m_desc; }
    const ProbeData& data() const { return m_data; }
    const ProbeVolume& volume() const { return m_volume; }
    const IrradianceCacheEntry& cacheEntry(u32 probe_index) const;
    const DDGIUpdateStats& lastUpdateStats() const { return m_last_update; }
    const DdgiInfo& info() const { return m_info; }

    /// Update a rotating subset of probes — CUDA path when available, CPU reference otherwise.
    bool update(u32 frame_index, void* cuda_stream = nullptr);

    /// Sample nearest-probe irradiance (CPU stub for deferred shading integration).
    DDGISampleResult sampleIrradiance(const DDGISampleRequest& request) const;

private:
    void releaseResources();
    bool allocateResources(ResourceManager& resources);

    DDGIDesc m_desc{};
    ProbeData m_data{};
    ProbeVolume m_volume{};
    std::vector<IrradianceCacheEntry> m_cache;
    DDGIUpdateStats m_last_update{};
    DdgiInfo m_info{};
    ResourceManager* m_resources = nullptr;
    bool m_ready = false;
};

/// Why probe-update launch preflight rejected the request (B5.6 deepen).
/// Why a host probe-update launch preflight rejected the request (B5.6 deepen).
enum class DdgiLaunchRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NullIndices,
    ZeroCount,
    OutOfRangeIndex,
};

/// Human-readable label for launch reject reasons (logging / tests).
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason);

/// Preflight guard before host probe-update launch; false on empty grid or OOB indices.
bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);
/// Early-out when probe-update launch would be rejected — same ordering as `canLaunchDdgiProbeUpdate`.
bool wouldSkipDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);
/// Classify why host probe-update launch preflight would reject.
ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count);
bool wouldSkipDdgiProbeUpdate(const DDGIDesc& desc,
/// Early-out when probe-update launch would be rejected — same ordering as `wouldSkipDdgiProbeUpdate` (B5.6 deepen).
bool shouldSkipDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);
/// Probe-update launch preflight; false when launch would be rejected (B5.6 deepen).
bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason = nullptr);
/// Early-out when probe-update launch would be rejected — includes reject-reason diagnostics.
/// Early-out with reject-reason diagnostics (B5.6 deepen).
                              ProbeUpdateLaunchRejectReason& outReason);
/// Diagnose why probe-update launch preflight would reject; vacuously succeeds when launchable.
bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                               const u32* probe_indices,
                               u32 probe_count,
                               ProbeUpdateLaunchRejectReason& outReason);
/// Diagnose why scheduled probe indices would fail launch preflight.
bool tryValidateScheduledProbeIndices(const DDGIDesc& desc,
                                      const u32* probe_indices,
                                      u32 probe_count,
                                      ProbeUpdateLaunchRejectReason& outReason);
/// Early-out when scheduled probe indices would fail launch preflight.
bool wouldSkipScheduledProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);
/// Host probe-update launch preflight with optional reject-reason output (B5.6 deepen).
/// True when host probe-update launch preflight passes (B5.6 deepen).
bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason = nullptr);

/// Preflight guard before DDGI probe-update launch.
/// Diagnose why probe-update launch would reject.
bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              DdgiLaunchRejectReason& outReason);
/// Preflight guard for probe update launch — non-empty grid, non-null indices, in-range probe indices.
/// Diagnose why launch preflight would reject; vacuously succeeds on valid requests.
                                 DdgiLaunchRejectReason& out_reason);
/// Count probe indices that exceed the valid probe range; returns 0 on empty grid or null buffer.
u32 countInvalidLaunchProbeIndices(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);
/// Diagnose why launch preflight would reject; vacuously succeeds when launch is allowed.
/// Diagnose why launch preflight would reject (B5.6 deepen).
/// Diagnose why probe-update launch preflight would reject.
                                 LaunchRejectReason& outReason);

/// Host launcher for probe trace + blend kernels — stub until CUDA kernels land.
bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream = nullptr);
/// Launch probe update with reject-reason diagnostics; false when preflight rejects.
bool tryLaunch_ddgi_probe_update(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 void* cuda_stream,
                                 ProbeUpdateLaunchRejectReason& outReason);

} // namespace fuse::renderer
