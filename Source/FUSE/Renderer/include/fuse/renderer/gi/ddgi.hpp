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

/// Continuous octahedral tile sample coordinates for bilinear irradiance lookup.
struct DdgiTileBilinearCoords {
    u32 texel_u0 = 0;
    u32 texel_v0 = 0;
    u32 texel_u1 = 0;
    u32 texel_v1 = 0;
    f32 tu = 0.f;
    f32 tv = 0.f;
};

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

/// Per-kind probe counts for border shell classification (B5.6 deepen).
struct ProbeBorderCounts {
    u32 total = 0;
    u32 interior = 0;
    u32 border = 0;
    u32 face = 0;
    u32 edge = 0;
    u32 corner = 0;
};

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
    static bool isValidProbeIndex(const DDGIDesc& desc, u32 probe_index);
    static bool isBorderProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Face/edge/corner shell classification; `Invalid` when coord or grid is empty.
    static ProbeBorderKind probeBorderKind(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static ProbeValidityFlags probeValidity(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static ProbeValidityFlags probeValidityFromIndex(const DDGIDesc& desc, u32 probe_index);
    /// Validity for a flat probe index after `clampProbeIndex` (safe for OOB scheduling).
    static ProbeValidityFlags probeValidityFromClampedIndex(const DDGIDesc& desc, u32 probe_index);
    /// True when `probe_index` exceeds the valid probe range (would be clamped).
    static bool isProbeIndexOutOfRange(u32 probe_index, const DDGIDesc& desc);
    /// Clamp a flat probe index to [0, probeCount - 1]; returns 0 when the grid is empty.
    static u32 clampProbeIndex(u32 probe_index, const DDGIDesc& desc);
    static u32 clampProbeCoordX(u32 x, const DDGIDesc& desc);
    static u32 clampProbeCoordY(u32 y, const DDGIDesc& desc);
    static u32 clampProbeCoordZ(u32 z, const DDGIDesc& desc);
    /// Clamp each axis then encode a flat probe index; returns 0 on empty grid.
    static u32 probeIndexFromClampedCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Clamp trilinear corner indices/weights to grid bounds (no-op on empty grid).
    static void clampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords);
    /// Ensure corner indices are ordered (x0≤x1, …) and weights stay in [0, 1].
    static void normalizeProbeSampleCoords(ProbeSampleCoords& coords);
    /// True when corner indices lie within the grid and weights are in [0, 1].
    static bool isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords);
    /// Build trilinear corner indices/weights from a world position; false when grid is empty.
    static bool buildProbeSampleCoords(const DDGIDesc& desc,
                                       const fuse::math::Vec3& world_position,
                                       ProbeSampleCoords& out_coords);
    /// Fractional grid coordinates — origin cell centre is (0,0,0).
    static fuse::math::Vec3 worldToProbeGridCoord(const DDGIDesc& desc,
                                                  const fuse::math::Vec3& world_position);
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
/// Minimum irradiance-cache entries for trilinear sampling; 0 when the grid is not sampleable.
u32 requiredCacheCount(const DDGIDesc& desc);
/// True when `cache_count` covers every probe in `desc`.
bool isCacheSizedForGrid(const DDGIDesc& desc, u32 cache_count);
/// Probe-cache shortfall vs `requiredCacheCount`; 0 when sized or the grid is not sampleable.
u32 cacheEntriesMissing(const DDGIDesc& desc, u32 cache_count);
/// Combined probe-index + cache-length guard for cache lookups.
bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count);
/// Sample-request guard — grid ready and cache sized for trilinear lookup (empty normals resolve at sample time).
bool isValidSampleRequest(const DDGIDesc& desc,
                          const DDGISampleRequest& request,
                          u32 cache_count);
fuse::math::Vec3 probeWorldPosition(const DDGIDesc& desc, u32 probe_index);
/// World position after `clampProbeIndex` — safe for OOB scheduling indices.
fuse::math::Vec3 probeWorldPositionClamped(const DDGIDesc& desc, u32 probe_index);
u32 irradianceAtlasWidth(const DDGIDesc& desc);
u32 irradianceAtlasHeight(const DDGIDesc& desc);
u32 depthAtlasWidth(const DDGIDesc& desc);
u32 depthAtlasHeight(const DDGIDesc& desc);
void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count);
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

/// Preflight guard before host probe-update launch; false on empty grid or OOB indices.
bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count);

/// Host launcher for probe trace + blend kernels — stub until CUDA kernels land.
bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream = nullptr);

} // namespace fuse::renderer
