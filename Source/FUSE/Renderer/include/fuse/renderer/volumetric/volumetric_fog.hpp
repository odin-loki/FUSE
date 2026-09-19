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

/// Why a froxel sample-coord preflight rejected the request (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRange,
    DepthOutOfRange,
/// Why sample-coord mapping or bounds checks rejected a froxel request (B5.11 deepen).
    OutOfBounds,
    InvalidDepth,
};

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);
/// Why froxel camera validation rejected the request (B5.11 deepen).
enum class FroxelCameraRejectReason : u8 {
    InvalidNearPlane,
    InvalidFarPlane,
    InvertedDepthRange,

/// Human-readable label for froxel camera reject reasons (logging / tests).
const char* froxelCameraRejectReasonLabel(FroxelCameraRejectReason reason);

/// Exponential depth-slice bounds — shared by froxel injection and CPU tests.
struct FroxelSliceLayout {
    static bool isCameraValid(const FroxelCameraDesc& camera);
    static bool tryValidateCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason);
    static bool isSliceIndexOutOfRange(u32 sliceZ, const FroxelGridDesc& desc);
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

/// True when a screen-mapping reject reason would block mapping (B5.11 deepen pass).
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason);

/// Why froxel sample-coord preflight rejected the request (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    OutOfBounds,
    InvalidWeights,
/// Why froxel grid desc validation rejected the request (B5.11 deepen).
enum class FroxelGridRejectReason : u8 {
    EmptyTilesX,
    EmptyTilesY,
    EmptySlicesZ,

/// Human-readable label for froxel grid reject reasons (logging / tests).
const char* froxelGridRejectReasonLabel(FroxelGridRejectReason reason);

/// Why sample-coord validation rejected the request (B5.11 deepen).
    TileOutOfRange,
    WeightOutOfRange,

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// True when a sample-coord reject reason would block sampling (B5.11 deepen pass).
bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);

/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    InaccessibleGrid,
    InvalidSampleCoords,
    ClampableWeights,

/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason);

/// True when a trilinear sample reject reason would block sampling (B5.11 deepen pass).
bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason);
/// Why a screen-depth → sample-coord mapping rejected the request (B5.11 deepen).
enum class FroxelSampleRejectReason : u8 {

/// Human-readable label for sample reject reasons (logging / tests).
const char* froxelSampleRejectReasonLabel(FroxelSampleRejectReason reason);

/// Why screen-depth → froxel sample coord mapping was rejected (B5.11 deepen).

/// Human-readable label for sample coord reject reasons (logging / tests).
/// Why screen-depth sample coord mapping rejected the request (B5.11 deepen).
enum class FroxelSampleCoordRejectReason : u8 {

const char* froxelSampleCoordRejectReasonLabel(FroxelSampleCoordRejectReason reason);
/// Why screen-depth → froxel mapping rejected the request (B5.11 deepen).
enum class FroxelScreenMappingRejectReason : u8 {
    DepthBelowNear,
    DepthAboveFar,

const char* froxelScreenMappingRejectReasonLabel(FroxelScreenMappingRejectReason reason);

/// Why sample-coord preflight rejected froxel density interpolation (B5.11 deepen).
    OutOfRange,
    InvertedCorners,


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
    /// True when tile/slice coords exceed grid bounds (would be clamped).
    static bool isCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
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
    /// Clamp sample coords in place; returns false on an empty grid.
    /// Clamp sample coords; returns false without modifying `coords` on an empty grid.
    /// Clamp sample coords; returns false and leaves `coords` unchanged on an empty grid.
    /// True when sample corner indices and interpolation weights are within grid bounds.
    static bool isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Clamp sample coords; returns false without modifying coords on an empty grid.
    /// Clamp sample coords; returns false on empty grid (coords left unchanged).
    static bool tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Screen-depth → sample coords with reject-reason diagnostics.
    static bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const FroxelGridDesc& desc,
                                                const FroxelCameraDesc& camera,
                                                FroxelSampleCoords& outCoords,
                                                SampleCoordRejectReason& outReason);
    /// Clamp sample coords in place with reject-reason diagnostics.
    static bool tryClampSampleCoords(FroxelSampleCoords& coords,
    /// True when `viewDepth` lies within the camera near/far range.
    static bool canMapScreenDepth(f32 viewDepth, const FroxelCameraDesc& camera);
    /// Bounds-check sample coords with reject-reason diagnostics.
    static bool tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
    /// Ensure corner indices are ordered (x0≤x1, …) and weights stay in [0, 1].
    static void normalizeFroxelSampleCoords(FroxelSampleCoords& coords);
    /// True when corner indices lie within the grid, are ordered, and weights are in [0, 1].
    static bool isValidFroxelSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Build trilinear corner indices/weights from screen depth; false when grid is empty or depth is rejected.
    static bool buildFroxelSampleCoords(f32 screenX,
                                        FroxelSampleCoords& outCoords);
    /// Clamp interpolation weights to [0, 1] without modifying tile/slice corner indices.
    static void normalizeSampleCoords(FroxelSampleCoords& coords);
    /// Diagnose why sample coords fail bounds checks; vacuously succeeds when in bounds.
    static bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
    /// Normalize corner ordering and clamp interpolation weights to [0, 1].
    /// True when corners, ordering, and weights are valid for trilinear lookup.
    static bool isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Preflight sample coords against grid bounds; vacuously succeeds when coords are valid.
    static bool tryCanSampleAtCoords(const FroxelSampleCoords& coords,
    static bool mapScreenDepthToSampleCoords(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const FroxelGridDesc& desc,
                                             const FroxelCameraDesc& camera,
                                             FroxelSampleCoords& outCoords);
    /// Screen-depth → sample coords with reject-reason diagnostics.
    /// Screen-depth mapping with guard diagnostics; false when mapping would fail.
    static bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const FroxelGridDesc& desc,
                                                const FroxelCameraDesc& camera,
                                                FroxelSampleCoords& outCoords,
                                                ScreenMappingRejectReason& outReason);
                                                FroxelSampleRejectReason& outReason);
                                                SampleCoordRejectReason& outReason);
                                                FroxelSampleCoordRejectReason& outReason);
    /// Screen-depth → linear froxel index (mirrors clustered `mapScreenDepthToClusterIndex`).
    static bool mapScreenDepthToFroxelIndex(f32 screenX,
                                            f32 screenY,
                                            f32 viewDepth,
                                            const FroxelGridDesc& desc,
                                            const FroxelCameraDesc& camera,
                                            u32& outFroxelIndex);
    /// Screen-depth → froxel index with reject-reason diagnostics.
    /// Screen-depth → froxel index with guard diagnostics; false when mapping would fail.
    /// Screen-depth → sample coords with reject-reason diagnostics; leaves `outCoords` untouched on failure.
    static bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const FroxelGridDesc& desc,
                                                const FroxelCameraDesc& camera,
                                                FroxelSampleCoords& outCoords,
                                                FroxelScreenMappingRejectReason& outReason);
    /// Screen-depth → froxel index with reject-reason diagnostics; leaves `outFroxelIndex` untouched on failure.
    static bool tryMapScreenDepthToFroxelIndex(f32 screenX,
                                               f32 screenY,
                                               f32 viewDepth,
                                               const FroxelGridDesc& desc,
                                               const FroxelCameraDesc& camera,
                                               u32& outFroxelIndex,
                                               ScreenMappingRejectReason& outReason);
    /// True when interpolation weights or corner indices would be clamped before sampling.
    static bool wouldClampSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Grid-only sample-coord preflight; false on empty desc or hard OOB corners.
    static bool tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                         SampleCoordRejectReason& outReason);
    /// Grid-only sample-coord preflight without reject-reason diagnostics.
    static bool canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Early-out when sample-coord preflight would be rejected — same ordering as `tryPreflightSampleCoords`.
    static bool wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Classify why sample-coord preflight would reject — same ordering as `tryPreflightSampleCoords`.
    static SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                             const FroxelGridDesc& desc);
    /// Non-mutating sample-coord preflight — returns true when sampling would proceed.
    static bool preflightSampleCoords(const FroxelSampleCoords& coords,
                                      SampleCoordRejectReason* reason = nullptr);
    /// Classify why screen-depth → sample coords would reject — same ordering as `tryMapScreenDepthToSampleCoords`.
    static ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
                                                                 const FroxelCameraDesc& camera);
    /// Non-mutating screen-mapping preflight — returns true when mapping would proceed.
    static bool preflightScreenMapping(f32 screenX,
                                       FroxelSampleCoords* outCoords = nullptr,
                                       ScreenMappingRejectReason* reason = nullptr);
                                               FroxelSampleRejectReason& outReason);
                                               FroxelSampleCoordRejectReason& outReason);
                                               FroxelScreenMappingRejectReason& outReason);
};

/// Why grid density validation rejected a froxel cache (B5.11 deepen).
enum class GridDensityRejectReason : u8 {
    None = 0,
    EmptyDesc,
    EmptyGrid,
    DescMismatch,
    UndersizedStorage,
    DescMismatch,
    DensityCountMismatch,
};

/// Human-readable label for density reject reasons (logging / tests).
const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason);

/// True when a grid-density reject reason would block validation (B5.11 deepen pass).
bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason);

/// Why analytic froxel populate would skip meaningful fill (B5.11 deepen).
enum class FroxelPopulateRejectReason : u8 {
    InvalidCamera,
    ZeroDensity,
    ZeroMarchSteps,

/// Human-readable label for populate reject reasons (logging / tests).
const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// Why a froxel density lookup preflight rejected the request (B5.11 deepen).
enum class DensityLookupRejectReason : u8 {
    EmptyGrid,
    EmptyStorage,
    IndexOutOfRange,
    None = 0,
    DescMismatch,
    SampleCoordsOutOfRange,
};

/// Human-readable label for density lookup reject reasons (logging / tests).
const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason);

/// True when a density lookup reject reason would block lookup (B5.11 deepen pass).
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason);
/// Why density-grid validation rejected a froxel population check (B5.11 deepen).
enum class DensityGridRejectReason : u8 {
    EmptyGridDesc,
    CountPartitionMismatch,

/// Human-readable label for density-grid reject reasons (logging / tests).
const char* densityGridRejectReasonLabel(DensityGridRejectReason reason);
/// Why grid density validation rejected a froxel density buffer (B5.11 deepen).
enum class FroxelDensityRejectReason : u8 {

const char* froxelDensityRejectReasonLabel(FroxelDensityRejectReason reason);
/// Why a froxel lookup preflight rejected the request (B5.11 deepen).
enum class FroxelLookupRejectReason : u8 {
};

/// Human-readable label for lookup reject reasons (logging / tests).
const char* froxelLookupRejectReasonLabel(FroxelLookupRejectReason reason);
/// Non-mutating froxel density grid preflight snapshot (B5.11 deepen).
struct FroxelGridPreflight {
    bool desc_non_empty = false;
    bool storage_allocated = false;
    bool storage_matches_desc = false;
    bool density_counts_valid = false;

    bool can_lookup() const { return desc_non_empty && storage_allocated && storage_matches_desc; }
    bool can_validate() const { return can_lookup() && density_counts_valid; }

/// Populate preflight outcome for analytic froxel injection (B5.11 deepen).
struct FroxelPopulatePreflight {
    bool params_enabled = false;

    bool can_populate() const { return desc_non_empty && params_enabled; }
/// Why sample-coord preflight rejected froxel interpolation coords (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRange,

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

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
/// Diagnose why froxel grid desc validation would reject; vacuously succeeds on non-empty desc.
bool tryValidateFroxelGridDesc(const FroxelGridDesc& desc, FroxelGridRejectReason& outReason);
/// Early-out when analytic fog populate should be skipped (empty grid, invalid camera, or zero params).
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params);
/// Early-out when analytic froxel populate should be skipped for empty desc or disabled params.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params);
/// Preflight guard before index-based density lookup; false on empty grid or desc mismatch.
bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Preflight guard before tile/slice coord density lookup; false on empty grid or desc mismatch.
bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 tileX,
                      u32 tileY,
                      u32 sliceZ);
/// Classify why index-based density lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index);
/// Classify why coord-based density lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
/// Non-mutating index-based density lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 index,
                            DensityLookupRejectReason* reason = nullptr);
/// Non-mutating coord-based density lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
/// Diagnose why lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         DensityLookupRejectReason& outReason);
/// Diagnose why coord lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
/// Early-out when density lookup would be rejected — same ordering as `tryCanLookupAtIndex`.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when index-based density lookup would be rejected; OOB indices that clamp are not skipped.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Early-out when coord-based density lookup would be rejected; OOB coords that clamp are not skipped.
bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// True when a lookup at `index` would clamp into the valid froxel range.
bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc);
/// True when a lookup at tile/slice coords would clamp into the valid froxel range.
bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
/// Preflight guard before coord-based density sampling; false on inaccessible grid or invalid coords.
bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& coords);
/// Diagnose why coord-based sample preflight would reject.
bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords,
                          SampleCoordRejectReason& outReason);
/// Preflight guard before trilinear density sampling; false on inaccessible grid or hard OOB coords.
bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              FroxelTrilinearSampleRejectReason* reason = nullptr);
/// Diagnose why trilinear sample preflight would reject; warns on clampable weights.
bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason);
/// Early-out when trilinear density sampling would be rejected — same ordering as `tryCanTrilinearSampleAtCoords`.
bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
bool canSampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Preflight guard before index-based density access; false on empty grid or desc mismatch.
bool canSampleAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Preflight guard before tile/slice density access; false on empty grid or desc mismatch.
bool canSampleAtCoord(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when froxel populate/injection should be skipped for an empty desc.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc);
                         FroxelLookupRejectReason& outReason);
/// True when the grid/desc pair cannot support any density sample (empty desc or storage).
bool isEmptyGridForSampling(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Preflight guard before coord-based density sampling; false on inaccessible grid or OOB coords.
                       const FroxelGridDesc& desc,
/// Diagnose why coord-based sampling preflight would reject.
/// Early-out when density sampling should be skipped for an empty grid or OOB sample coords.
bool shouldSkipDensitySample(const FroxelDensityGrid& grid,
/// Preflight guard before tile/slice coord density lookup; false on empty grid or desc mismatch.
bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      u32 tileX,
                      u32 tileY,
                      u32 sliceZ);
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason);
/// True when the froxel grid can participate in spatial density sampling.
bool canSampleFroxelGrid(const FroxelGridDesc& desc);
/// Early-out when bilinear/trilinear density sampling should be skipped.
bool shouldSkipFroxelSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Combined grid-access + sample-coord guard for density interpolation.
bool isValidDensitySampleRequest(const FroxelDensityGrid& grid,
/// True when analytic fog populate can allocate into a non-empty froxel desc.
bool canPopulateFroxelGrid(const FroxelGridDesc& desc);
/// Diagnose lookup preflight including froxel index bounds (B5.11 deepen).
bool tryCanLookupAtIndexBounds(const FroxelDensityGrid& grid,
                               u32 index,
/// Diagnose lookup preflight for trilinear sample coords (B5.11 deepen).
bool tryCanLookupAtSampleCoords(const FroxelDensityGrid& grid,
/// Non-mutating grid accessibility + density-count preflight (B5.11 deepen).
FroxelGridPreflight preflightFroxelDensityGrid(const FroxelDensityGrid& grid,
                                               f32 epsilon = 1e-6f);
/// Populate preflight — does not allocate or mutate grid storage (B5.11 deepen).
FroxelPopulatePreflight preflightPopulateFroxelGrid(const FroxelGridDesc& desc,
                                                    const VolumetricFogParams& params);
/// Stricter lookup preflight that also rejects out-of-range flat indices.
bool canLookupAtIndexInRange(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Diagnose strict lookup preflight rejection including index range checks.
bool tryCanLookupAtIndexInRange(const FroxelDensityGrid& grid,
/// True when at least one froxel exceeds `epsilon`; false when storage is empty.
bool hasNonZeroDensity(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Early-out when the grid is inaccessible or uniformly below `epsilon`.
bool shouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// True when the grid has storage, the desc is non-empty, and storage matches the desc.
bool canAccessDensityGrid(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Preflight guard before index-based density access; false on empty grid or desc mismatch.
bool canAccessDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Count froxels with density above `epsilon`; returns 0 when the grid is empty.
u32 countNonZeroFroxels(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Count froxels with density at or below `epsilon`; returns 0 when the grid is empty.
u32 countEmptyFroxels(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Read density at a clamped flat froxel index; returns 0 when access is denied.
f32 sampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Lookup with guard preflight; returns false when `canSampleDensityAtIndex` would reject the request.
/// Sample with guard preflight; returns false when `canSampleAtIndex` would reject the request.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity);
/// Read density at clamped tile/slice coords; returns 0 when grid/desc mismatch or empty.
f32 sampleDensityAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ);
/// Sample with guard preflight; returns false when `canAccessDensityAtIndex` would reject the request.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             u32 index,
/// Sample with guard preflight; returns false when `canSampleAtCoord` would reject the request.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity);
/// Write density at a clamped flat froxel index; returns false when grid/desc mismatch or empty.
/// Write density at a clamped flat froxel index; returns false when access is denied.
bool writeDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value);
/// Write with guard preflight; returns false when `canSampleDensityAtIndex` would reject the request.
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value);
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
/// Classify why grid density validation would reject — same ordering as `tryValidateGridDensity`.
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  f32 epsilon = 1e-6f);
/// Non-mutating grid-density preflight — returns true when validation would succeed.
bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason = nullptr,
/// Diagnose the first density invariant that fails; vacuously succeeds when `desc` is empty.
bool tryValidateGridDensity(const FroxelDensityGrid& grid,
                            GridDensityRejectReason& outReason,
                            f32 epsilon = 1e-6f);
/// Diagnose density validation against the clamped froxel count derived from `desc`.
bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
/// Diagnose density validation rejecting empty desc (B5.11 deepen).
bool tryValidateGridDensityStrict(const FroxelDensityGrid& grid,
                                  f32 epsilon = 1e-6f);
/// Diagnose density validation against `desc`; vacuously succeeds on empty grids.
/// True when froxel analytic populate should run for the given desc and fog params.
bool canPopulateFroxelGrid(const FroxelGridDesc& desc, const VolumetricFogParams& params);
/// Diagnose sample-coord bounds before bilinear/trilinear density lookup.
bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
                             SampleCoordRejectReason& outReason);
/// Read density with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             u32 index,
                             f32& outDensity);
/// Read density with guard preflight and reject-reason diagnostics.
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
/// Write density with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
/// Write density with guard preflight and reject-reason diagnostics.
                            f32 value,
/// Read density at clamped tile/slice coords with guard preflight.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
/// Read density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             FroxelLookupRejectReason& outReason);
                            f32 value);
                             f32& outDensity);
/// Read density with guard preflight and reject-reason diagnostics.
/// Read density with guard preflight and lookup reject-reason diagnostics.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
                             FroxelLookupRejectReason& outReason);
/// Read density with index-bounds guard preflight (B5.11 deepen).
bool trySampleDensityAtIndexBounds(const FroxelDensityGrid& grid,
/// Write density with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
/// Write density with guard preflight and reject-reason diagnostics.
                            f32 value,
/// Read density at clamped tile/slice coords with guard preflight.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
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
                             f32& outDensity);
/// Read density at tile/slice coords with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
/// Write density at clamped tile/slice coords with guard preflight.
bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
/// Write density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
/// Desc match plus density-count invariant; vacuously true for empty storage or empty froxel desc.
/// Write with guard preflight; returns false when `canAccessDensityAtIndex` would reject the request.
bool tryValidateDensityCounts(const FroxelDensityGrid& grid,
                              DensityGridRejectReason& outReason,
/// Validate density counts against the clamped froxel count derived from `desc`.
bool validateDensityCountsForDesc(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            FroxelDensityRejectReason& outReason,
f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords);
/// Bilinear sample with guard preflight; returns false when lookup would be rejected.
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value);
/// Write density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            f32 value,
                            FroxelLookupRejectReason& outReason);
/// Bilinear density sample with guard preflight; returns false when the grid is inaccessible.
bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity);
/// Bilinear sample with guard preflight and reject-reason diagnostics.
                              f32& outDensity,
                              SampleCoordRejectReason& outReason);
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            DensityLookupRejectReason& outReason);
f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords);
/// Bilinear density sample with guard preflight; returns false when lookup would be rejected.
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
                              DensityLookupRejectReason& outReason);
f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelSampleCoords& coords);
/// Trilinear sample with guard preflight; returns false when lookup would be rejected.
/// Trilinear density sample with guard preflight; returns false when the grid is inaccessible.
/// Trilinear density sample with guard preflight; returns false when lookup would be rejected.
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
/// Trilinear sample with guard preflight and trilinear-specific reject-reason diagnostics.
                               FroxelTrilinearSampleRejectReason& outReason);
                               DensityLookupRejectReason& outReason);
/// Bilinear sample with sample-coord guard preflight (B5.11 deepen).
bool trySampleDensityBilinearAtCoords(const FroxelDensityGrid& grid,
/// Trilinear sample with sample-coord guard preflight (B5.11 deepen).
bool trySampleDensityTrilinearAtCoords(const FroxelDensityGrid& grid,
/// Bilinear sample with strict preflight (grid accessible + sample coords in bounds).
bool trySampleDensityBilinearInBounds(const FroxelDensityGrid& grid,
/// Trilinear sample with strict preflight (grid accessible + sample coords in bounds).
bool trySampleDensityTrilinearInBounds(const FroxelDensityGrid& grid,
/// True when grid is accessible for coord-based density sampling.
bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& coords);
/// Diagnose coord-based sampling preflight; reports lookup and sample-coord reject reasons.
bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason);
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
/// Screen-space trilinear density sample with guard diagnostics.
/// Screen-space trilinear density sample with guard preflight and reject-reason diagnostics.
/// Screen-space sample with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              ScreenMappingRejectReason& outReason);
                              FroxelSampleRejectReason& outReason);
                              FroxelLookupRejectReason& outLookupReason,
                              FroxelSampleCoordRejectReason& outCoordReason);
                              SampleCoordRejectReason& outReason);
                              DensityLookupRejectReason& outReason);
void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params);
/// Early-out when populate would allocate but skip the analytic fill loop.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params);
/// True when analytic populate would write non-zero froxel density for a valid camera.
bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
/// Non-mutating analytic populate preflight — returns true when meaningful fill would proceed.
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Diagnose why analytic populate would skip meaningful fill.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   FroxelPopulateRejectReason& outReason);
/// Early-out when analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
/// Guarded populate — always mirrors `populateFromAnalyticFog`; returns false when preflight rejects fill.
/// Populate with guard preflight; returns false when `canPopulateFroxelGrid` would reject the request.
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
/// Guarded populate with reject-reason diagnostics.
/// Populate with preflight guards; returns false without modifying `grid` when populate would be skipped.
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params,
                                FroxelPopulateRejectReason& outReason);
                                FroxelGridRejectReason& outGridReason,
                                FroxelCameraRejectReason& outCameraReason);
} // namespace froxel_util

/// CPU stub — exponential height falloff density sample (P5 acceptance reference).
f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos);

/// Records logical volumetric fog work for the frame; returns false when disabled.
bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats);

/// Render-graph hook — inserts the volumetric fog CUDA pass after screen-space AO.
void resetVolumetricFogPassGraphStorage();
void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count);

} // namespace fuse::renderer
