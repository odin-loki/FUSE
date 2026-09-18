#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Volumetric fog parameters (B5.11 — P5 §5.11).
struct VolumetricFogParams {
    f32 density = 0.02f;
    f32 anisotropy = 0.3f;
    math::Vec3 fog_color{0.8f, 0.85f, 0.9f};
    f32 height_falloff = 0.2f;
    f32 base_height = 0.f;
    u32 march_steps = 32;
    bool receive_shadows = true;
};

struct VolumetricFogPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    f32 lastDensity = 0.f;
};

/// Froxel grid configuration (B5.11 follow-up — view-aligned volumetric injection grid).
struct FroxelGridDesc {
    static constexpr u32 kMaxTilesX = 32u;
    static constexpr u32 kMaxTilesY = 18u;
    static constexpr u32 kMaxSlicesZ = 128u;

    u32 tilesX = 16;
    u32 tilesY = 9;
    u32 slicesZ = 64;

    u32 froxelCount() const { return tilesX * tilesY * slicesZ; }
    bool isEmpty() const { return froxelCount() == 0u; }
    /// Last valid flat froxel index; returns 0 when the grid has no froxels.
    u32 maxFroxelIndex() const {
        const u32 count = froxelCount();
        return count == 0u ? 0u : count - 1u;
    }

    /// Clamp tile/slice counts to CPU stub limits; zero dimensions remain zero (empty grid).
    static FroxelGridDesc clampCounts(const FroxelGridDesc& raw);
};

/// Camera inputs for froxel depth-slice distribution and screen mapping.
struct FroxelCameraDesc {
    math::Vec3 position{};
    f32 nearPlane = 0.1f;
    f32 farPlane = 100.f;
    u32 screenWidth = 1920;
    u32 screenHeight = 1080;
};

/// CPU-side froxel density cache — per-froxel scalar density for stub injection and tests.
struct FroxelDensityGrid {
    std::vector<f32> density;

    /// Resize density storage to match a clamped froxel grid; zero-fills all froxels.
    void allocate(const FroxelGridDesc& desc);
    /// Drop all froxel density storage.
    void clear();
    bool isEmpty() const { return density.empty(); }
    /// True when density storage matches the clamped froxel count for `desc`.
    bool matchesDesc(const FroxelGridDesc& desc) const;
};

/// Continuous froxel sample coordinates for trilinear density lookup.
struct FroxelSampleCoords {
    u32 tileX0 = 0;
    u32 tileY0 = 0;
    u32 tileX1 = 0;
    u32 tileY1 = 0;
    u32 sliceZ0 = 0;
    u32 sliceZ1 = 0;
    f32 tx = 0.f;
    f32 ty = 0.f;
    f32 tz = 0.f;
};

/// Exponential depth-slice bounds — shared by froxel injection and CPU tests.
struct FroxelSliceLayout {
    static f32 computeSliceNearZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera);
    static f32 computeSliceFarZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera);
    static u32 computeSliceZFromDepth(f32 viewDepth, const FroxelGridDesc& desc, const FroxelCameraDesc& camera);
};

/// Why screen-depth → froxel mapping was rejected (B5.11 deepen).
enum class ScreenMappingRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InvalidCamera,
    DepthOutOfRange,
};

/// Human-readable label for screen-mapping reject reasons (logging / tests).
const char* screenMappingRejectReasonLabel(ScreenMappingRejectReason reason);

/// Why froxel sample-coord preflight rejected the request (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfBounds,
    InvalidWeights,
};

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// Froxel grid indexing helpers — mirrors clustered light layout (B5.4).
struct FroxelGridLayout {
    static bool isEmptyGrid(const FroxelGridDesc& desc);
    static u32 froxelIndex(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    /// Tile/slice coords clamped to grid bounds before linear index encode.
    static u32 froxelIndexClamped(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    static void decodeFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& tileX, u32& tileY, u32& sliceZ);
    static bool isValidFroxelIndex(u32 index, const FroxelGridDesc& desc);
    /// True when `index` exceeds the valid froxel range (would be clamped).
    static bool isFroxelIndexOutOfRange(u32 index, const FroxelGridDesc& desc);
    static u32 clampFroxelIndex(u32 index, const FroxelGridDesc& desc);
    static u32 clampTileX(u32 tileX, const FroxelGridDesc& desc);
    static u32 clampTileY(u32 tileY, const FroxelGridDesc& desc);
    static u32 clampSliceZ(u32 sliceZ, const FroxelGridDesc& desc);
    /// Last valid flat froxel index; returns 0 when the grid has no froxels.
    static u32 maxFroxelIndex(const FroxelGridDesc& desc);
    /// True when `index` equals the last valid froxel index for a non-empty grid.
    static bool isAtMaxFroxelIndex(u32 index, const FroxelGridDesc& desc);
    /// Clamp `index` into range; returns false and zeroes `outIndex` on an empty grid.
    static bool tryClampFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& outIndex);
    /// True when tile/slice corners and interpolation weights are within grid bounds.
    static bool areSampleCoordsInBounds(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when corners are ordered (tileX0≤tileX1, …) and weights lie in [0, 1].
    static bool isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when sample coords exceed grid bounds or interpolation weights are outside [0, 1].
    static bool isSampleCoordsOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Ensure corner indices are ordered and interpolation weights stay in [0, 1].
    static void normalizeSampleCoords(FroxelSampleCoords& coords);
    /// Clamp interpolation weights and corner indices to grid bounds.
    static void clampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Clamp sample coords in place; returns false without modifying `coords` on an empty grid.
    static bool tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    static bool mapScreenDepthToSampleCoords(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const FroxelGridDesc& desc,
                                             const FroxelCameraDesc& camera,
                                             FroxelSampleCoords& outCoords);
    /// Screen-depth → sample coords with reject-reason diagnostics.
    static bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const FroxelGridDesc& desc,
                                                const FroxelCameraDesc& camera,
                                                FroxelSampleCoords& outCoords,
                                                ScreenMappingRejectReason& outReason);
    /// Screen-depth → linear froxel index (mirrors clustered `mapScreenDepthToClusterIndex`).
    static bool mapScreenDepthToFroxelIndex(f32 screenX,
                                            f32 screenY,
                                            f32 viewDepth,
                                            const FroxelGridDesc& desc,
                                            const FroxelCameraDesc& camera,
                                            u32& outFroxelIndex);
    /// Screen-depth → froxel index with reject-reason diagnostics.
    static bool tryMapScreenDepthToFroxelIndex(f32 screenX,
                                               f32 screenY,
                                               f32 viewDepth,
                                               const FroxelGridDesc& desc,
                                               const FroxelCameraDesc& camera,
                                               u32& outFroxelIndex,
                                               ScreenMappingRejectReason& outReason);
};

/// Why grid density validation rejected a froxel cache (B5.11 deepen).
enum class GridDensityRejectReason : u8 {
    None = 0,
    EmptyDesc,
    UndersizedStorage,
    DescMismatch,
    DensityCountMismatch,
};

/// Human-readable label for density reject reasons (logging / tests).
const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason);

/// Why a froxel density lookup preflight rejected the request (B5.11 deepen).
enum class DensityLookupRejectReason : u8 {
    None = 0,
    EmptyGrid,
    DescMismatch,
    EmptyStorage,
};

/// Human-readable label for density lookup reject reasons (logging / tests).
const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason);

/// Why analytic froxel populate was rejected (B5.11 deepen).
enum class FroxelPopulateRejectReason : u8 {
    None = 0,
    EmptyGrid,
    ZeroDensity,
    ZeroMarchSteps,
};

/// Human-readable label for populate reject reasons (logging / tests).
const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason);

/// CPU froxel density interpolation helpers — mirrors CUDA trilinear sample stub.
namespace froxel_util {
f32 lerpDensity(f32 a, f32 b, f32 t);
/// True when density storage matches the clamped froxel count for `desc`.
bool gridMatchesDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// True when `desc` is non-empty, storage is allocated, and sizes match.
bool isDensityGridAccessible(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when the grid is inaccessible for index-based density lookup.
bool shouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when froxel density populate/sample should be skipped for an empty desc.
bool shouldSkipFroxelGrid(const FroxelGridDesc& desc);
/// Preflight guard before index-based density lookup; false on empty grid or desc mismatch.
bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Diagnose why lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 index,
                         DensityLookupRejectReason& outReason);
/// Preflight guard before coord-based density sampling; false on inaccessible grid or invalid coords.
bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelGridDesc& desc,
                       const FroxelSampleCoords& coords);
/// Diagnose why coord-based sample preflight would reject.
bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords,
                          SampleCoordRejectReason& outReason);
/// True when lookup passes and sample coords are ordered with in-range weights.
bool canSampleValidCoords(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords);
/// Classify why a guarded froxel sample would skip — same ordering as `wouldSkipFroxelSample`.
SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelSampleCoords& coords);
/// Predict whether guarded bilinear/trilinear sampling would bail before interpolation.
bool wouldSkipFroxelSample(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         const FroxelSampleCoords& coords,
                         SampleCoordRejectReason* outReason = nullptr);
/// True when at least one froxel exceeds `epsilon`; false when storage is empty.
bool hasNonZeroDensity(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Early-out when the grid is inaccessible or uniformly below `epsilon`.
bool shouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Count froxels with density above `epsilon`; returns 0 when the grid is empty.
u32 countNonZeroFroxels(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Count froxels with density at or below `epsilon`; returns 0 when the grid is empty.
u32 countEmptyFroxels(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Read density at a clamped flat froxel index; returns 0 when grid/desc mismatch or empty.
f32 sampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Read density at clamped tile/slice coords; returns 0 when grid/desc mismatch or empty.
f32 sampleDensityAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ);
/// Write density at a clamped flat froxel index; returns false when grid/desc mismatch or empty.
bool writeDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value);
/// Write density at clamped tile/slice coords; returns false when grid/desc mismatch or empty.
bool writeDensityAtCoord(FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         f32 value);
/// True when non-zero + empty froxel counts sum to storage size (empty grid is vacuously true).
bool validateDensityCounts(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Count partition plus desc/storage agreement; vacuously true when `desc` is empty.
bool validateGridDensity(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Validate density against the clamped froxel count derived from `desc`.
bool validateGridDensityForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Diagnose the first density invariant that fails; vacuously succeeds when `desc` is empty.
bool tryValidateGridDensity(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            GridDensityRejectReason& outReason,
                            f32 epsilon = 1e-6f);
/// Diagnose density validation against the clamped froxel count derived from `desc`.
bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon = 1e-6f);
/// Read density with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity);
/// Read density with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
/// Write density with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value);
/// Write density with guard preflight and reject-reason diagnostics.
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value,
                            DensityLookupRejectReason& outReason);
/// Read density at clamped tile/slice coords with guard preflight.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity);
/// Read density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
/// Write density at clamped tile/slice coords with guard preflight.
bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value);
/// Write density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value,
                            DensityLookupRejectReason& outReason);
f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords);
/// Bilinear sample with guard preflight; returns false when lookup would be rejected.
bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity);
/// Bilinear sample with guard preflight and reject-reason diagnostics.
bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              SampleCoordRejectReason& outReason);
f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords);
/// Trilinear sample with guard preflight; returns false when lookup would be rejected.
bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity);
/// Trilinear sample with guard preflight and reject-reason diagnostics.
bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity,
                               SampleCoordRejectReason& outReason);
/// Screen-space trilinear density sample; returns 0 when mapping fails or grid is empty.
f32 sampleDensityAtScreen(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelCameraDesc& camera,
                          f32 screenX,
                          f32 screenY,
                          f32 viewDepth);
/// Screen-space sample with guard preflight; returns false when mapping or lookup fails.
bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity);
/// Screen-space sample with guard preflight and screen-mapping reject-reason diagnostics.
bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              ScreenMappingRejectReason& outReason);
/// Early-out when analytic froxel populate should be skipped for an empty desc or disabled params.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params);
/// Diagnose why analytic froxel populate would be rejected.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason);
void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params);
} // namespace froxel_util

/// CPU stub — exponential height falloff density sample (P5 acceptance reference).
f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos);

/// Records logical volumetric fog work for the frame; returns false when disabled.
bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats);

/// Render-graph hook — inserts the volumetric fog CUDA pass after screen-space AO.
void resetVolumetricFogPassGraphStorage();
void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count);

} // namespace fuse::renderer
