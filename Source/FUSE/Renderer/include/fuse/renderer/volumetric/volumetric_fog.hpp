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
    /// True when any dimension exceeds CPU stub maxima and would be clamped by `clampCounts`.
    static bool wouldClampCounts(const FroxelGridDesc& raw);
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
    ScreenCoordsOutOfRange,
    NonFiniteScreenCoords,
    NonFiniteDepth,
};

/// Human-readable label for screen-mapping reject reasons (logging / tests).
const char* screenMappingRejectReasonLabel(ScreenMappingRejectReason reason);

/// True when a screen-mapping reject reason would block mapping (B5.11 deepen pass).
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason);
/// Why froxel sample-coord validation rejected the request (B5.11 deepen).
enum class FroxelSampleCoordsRejectReason : u8 {
    None = 0,
    EmptyGrid,
    OutOfRangeIndices,
    OutOfRangeWeights,
    UnorderedCorners,
};

/// Human-readable label for sample-coord validation reject reasons (logging / tests).
const char* froxelSampleCoordsRejectReasonLabel(FroxelSampleCoordsRejectReason reason);
/// Classify why screen-depth → froxel mapping would be rejected (B5.11 deepen).
/// True when a screen-mapping reject reason blocks guarded sampling (B5.11 deepen).

/// Classify why screen-depth → sample-coord mapping would reject — same ordering as `tryMapScreenDepthToSampleCoords`.
ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera);
/// Screen-depth mapping preflight with optional reject-reason output (B5.11 deepen).
bool preflightScreenDepthMapping(f32 screenX,
                                 const FroxelCameraDesc& camera,
                                 ScreenMappingRejectReason* reason = nullptr);
/// True when a screen-mapping reject reason would block mapping (B5.11 deepen).
/// True when a screen-mapping reject reason would block froxel lookup (B5.11 deepen).

/// Why froxel sample-coord preflight rejected the request (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    None = 0,
    EmptyGrid,
    UnorderedCorners,
    OutOfBounds,
    InvalidCorners,
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
/// Why a froxel sample-coord preflight rejected the request (B5.11 deepen).
    DepthBelowNear,
    DepthAboveFar,
/// Why screen-depth → froxel sample-coord mapping rejected the request (B5.11 deepen).
    UnorderedCorners,
};

/// Why trilinear density sample preflight rejected the request (B5.11 deepen).
/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// True when a sample-coord reject reason would block sampling (B5.11 deepen pass).
bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);

/// True when a sample-coord reject reason would block sampling (B5.11 deepen).

/// Why froxel bilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelBilinearSampleRejectReason : u8 {

/// True when a sample-coord reject reason blocks guarded sampling (B5.11 deepen).



/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InaccessibleGrid,
    InvalidSampleCoords,
    ClampableWeights,
};

/// Human-readable label for bilinear sample reject reasons (logging / tests).
const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason);

/// True when a bilinear sample reject reason would block sampling (B5.11 deepen).
bool froxelBilinearSampleRejectReasonIsBlocking(FroxelBilinearSampleRejectReason reason);

/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InaccessibleGrid,
    InvalidSampleCoords,
    ScreenMappingFailed,
};

/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason);

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// True when a sample-coord reject reason would block sampling (B5.11 deepen pass).
bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);

/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    InaccessibleGrid,
    InvalidSampleCoords,
    ClampableWeights,
    None = 0,
    EmptyGrid,
    ClampRequired,
/// Why trilinear froxel density sampling preflight rejected the request (B5.11 deepen).
    NotAccessible,
    HardOutOfBounds,
    InvalidWeights,
    GridInaccessible,
/// Why coord-based froxel trilinear sampling preflight rejected the request (B5.11 deepen).
    DescMismatch,
    EmptyStorage,
};

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

const char* froxelScreenMappingRejectReasonLabel(FroxelScreenMappingRejectReason reason);

/// Why sample-coord preflight rejected froxel density interpolation (B5.11 deepen).
    OutOfRange,
    InvertedCorners,

/// Why froxel sample-coord validation rejected the request (B5.11 deepen follow-up).
    OutOfRangeTile,
    OutOfRangeWeight,


/// Why grid density validation rejected a froxel cache (B5.11 deepen).
enum class GridDensityRejectReason : u8 {
    EmptyDesc,
    UndersizedStorage,
    DensityCountMismatch,

/// Human-readable label for density reject reasons (logging / tests).
const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason);

/// Why a froxel density lookup preflight rejected the request (B5.11 deepen).
enum class DensityLookupRejectReason : u8 {
    SampleCoordRejected,
    ScreenMappingFailed,

/// Human-readable label for density lookup reject reasons (logging / tests).
const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason);
/// Why sample-coord bounds validation rejected the request (B5.11 deepen).
enum class SampleCoordBoundsRejectReason : u8 {
    OutOfBoundsTile,
    OutOfBoundsWeight,

/// Human-readable label for sample-coord bounds reject reasons (logging / tests).
const char* sampleCoordBoundsRejectReasonLabel(SampleCoordBoundsRejectReason reason);
enum class TrilinearSampleRejectReason : u8 {
    OutOfBounds,

const char* trilinearSampleRejectReasonLabel(TrilinearSampleRejectReason reason);

/// True when a sample-coord reject reason would block sampling (B5.11 deepen).

/// True when a trilinear sample reject reason would block sampling (B5.11 deepen).
/// True when a trilinear sample reject reason blocks guarded sampling (B5.11 deepen).

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
    static bool isFroxelCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    static u32 clampFroxelIndex(u32 index, const FroxelGridDesc& desc);
    static u32 clampTileX(u32 tileX, const FroxelGridDesc& desc);
    static u32 clampTileY(u32 tileY, const FroxelGridDesc& desc);
    static u32 clampSliceZ(u32 sliceZ, const FroxelGridDesc& desc);
    /// True when tile/slice coords exceed grid bounds (would be clamped before lookup).
    static bool isTileCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    /// Last valid flat froxel index; returns 0 when the grid has no froxels.
    static u32 maxFroxelIndex(const FroxelGridDesc& desc);
    /// True when `index` equals the last valid froxel index for a non-empty grid.
    static bool isAtMaxFroxelIndex(u32 index, const FroxelGridDesc& desc);
    /// Clamp `index` into range; returns false and zeroes `outIndex` on an empty grid.
    static bool tryClampFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& outIndex);
    /// True when tile/slice corners and interpolation weights are within grid bounds.
    static bool areSampleCoordsInBounds(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when tile/slice corners are ordered (tileX0≤tileX1, …).
    static bool areSampleCoordsOrdered(const FroxelSampleCoords& coords);
    /// True when corners are ordered (tileX0≤tileX1, …) and weights lie in [0, 1].
    static bool isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when sample coords exceed grid bounds or interpolation weights are outside [0, 1].
    static bool isSampleCoordsOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Diagnose why sample-coord validation would reject; vacuously succeeds on valid coords.
    static bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                        const FroxelGridDesc& desc,
                                        SampleCoordRejectReason& outReason);
    /// Empty-grid preflight before froxel grid operations; vacuously succeeds on non-empty grids.
    static bool tryPreflightNonEmptyGrid(const FroxelGridDesc& desc, GridDensityRejectReason& outReason);
                                        FroxelSampleCoordsRejectReason& outReason);
    /// Ensure corner indices are ordered and interpolation weights stay in [0, 1].
    /// True when corner indices are ordered and weights are within [0, 1] on a non-empty grid.
    static bool isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Swap reversed corner indices and mirror interpolation weights; clamps weights to [0, 1].
    static void normalizeSampleCoords(FroxelSampleCoords& coords);
    /// Diagnose why sample coords fail bounds check; vacuously succeeds on in-bounds coords.
    static bool tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
                                           const FroxelGridDesc& desc,
                                           SampleCoordRejectReason& outReason);
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
    /// Diagnose why sample-coord validation would reject; vacuously succeeds on valid coords.
    static bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                        const FroxelGridDesc& desc,
                                        SampleCoordRejectReason& outReason);
    /// Clamp sample coords with reject-reason diagnostics.
    static bool tryClampSampleCoords(FroxelSampleCoords& coords,
    /// Clamp sample coords in place with reject-reason diagnostics.
    /// Clamp sample coords with reject-reason diagnostics; false on empty desc or hard OOB corners.
    /// Classify sample-coord rejection — same ordering as `tryPreflightSampleCoords`.
    static SampleCoordRejectReason classifySampleCoordsReject(const FroxelSampleCoords& coords,
                                                              const FroxelGridDesc& desc);
    /// Classify screen-depth → sample-coord rejection — same ordering as `tryMapScreenDepthToSampleCoords`.
    static ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
                                                                  f32 screenY,
                                                                  f32 viewDepth,
                                                                  const FroxelCameraDesc& camera);
    /// Non-mutating screen-depth → sample-coord preflight with optional outputs.
    static bool preflightScreenDepthToSampleCoords(f32 screenX,
                                                   const FroxelCameraDesc& camera,
                                                   FroxelSampleCoords* outCoords = nullptr,
                                                   ScreenMappingRejectReason* outReason = nullptr);
    static bool mapScreenDepthToSampleCoords(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const FroxelCameraDesc& camera,
                                             FroxelSampleCoords& outCoords);
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
    /// Fix reversed corner ordering and clamp interpolation weights to [0, 1].
    /// Fix reversed tile/slice corners and clamp interpolation weights to [0, 1].
    static bool mapScreenDepthToSampleCoords(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const FroxelGridDesc& desc,
                                             const FroxelCameraDesc& camera,
                                             FroxelSampleCoords& outCoords);
    /// Screen-depth → sample coords with reject-reason diagnostics.
    /// Screen-depth mapping with guard diagnostics; false when mapping would fail.
    /// Screen-depth mapping with reject-reason diagnostics (B5.11 deepen).
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
                                               u32& outFroxelIndex,
                                               ScreenMappingRejectReason& outReason);
    /// True when screen X/Y lie outside [0, 1] and would be clamped before mapping.
    static bool wouldClampScreenCoords(f32 screenX, f32 screenY);
    /// Screen-depth mapping preflight without writing sample coords.
    static bool canMapScreenDepthToSampleCoords(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const FroxelGridDesc& desc,
                                                const FroxelCameraDesc& camera);
    /// Screen-depth → froxel index preflight without writing output index.
    static bool canMapScreenDepthToFroxelIndex(f32 screenX,
                                               f32 screenY,
                                               f32 viewDepth,
                                               const FroxelGridDesc& desc,
                                               const FroxelCameraDesc& camera);
    /// True when interpolation weights or corner indices would be clamped before sampling.
    static bool wouldClampSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when sample coords exceed bounds only through clampable weights or corners.
    static bool isSampleCoordsClampable(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Grid-only sample-coord preflight; false on empty desc or hard OOB corners.
    static bool tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                         SampleCoordRejectReason& outReason);
    /// Grid-only sample-coord preflight with optional reject-reason diagnostics (B5.11 deepen).
    static bool preflightSampleCoords(const FroxelSampleCoords& coords,
                                      const FroxelGridDesc& desc,
                                      SampleCoordRejectReason* reason = nullptr);
    /// Grid-only sample-coord preflight without reject-reason diagnostics.
    static bool canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Early-out when sample-coord preflight would be rejected — same ordering as `tryPreflightSampleCoords`.
    static bool wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Classify why sample-coord preflight would reject — same ordering as `tryPreflightSampleCoords`.
    /// Classify sample-coord rejection — same ordering as `tryPreflightSampleCoords`.
    /// Classify sample-coord preflight — same ordering as `tryPreflightSampleCoords`.
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
    /// Screen-depth → froxel index with reject-reason diagnostics (B5.11 deepen).
    /// True when tile/slice coords exceed grid bounds (would be clamped before lookup).
    static bool wouldClampTileCoords(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    /// Grid-only tile/slice preflight; false on empty desc or hard OOB coords.
    static bool tryPreflightTileCoords(u32 tileX,
                                       u32 tileY,
                                       u32 sliceZ,
                                       const FroxelGridDesc& desc,
                                       SampleCoordRejectReason& outReason);
    /// Grid-only tile/slice preflight without reject-reason diagnostics.
    static bool canPreflightTileCoords(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    /// Grid-only sample-coord preflight with optional reject-reason diagnostics (B5.11 deepen).
    /// Early-out when sample-coord preflight should be skipped for an empty desc (B5.11 deepen).
    static bool shouldSkipSampleCoordPreflight(const FroxelGridDesc& desc);
    /// True when sample coords are valid without clamping (B5.11 deepen).
    static bool sampleCoordsReady(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Normalize coords in place, then run grid-only sample-coord preflight.
    static bool tryNormalizeAndPreflightSampleCoords(FroxelSampleCoords& coords,
    /// Diagnose why sample-coord validation would reject; vacuously succeeds on valid coords.
    static bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
    /// Early-out when sample-coord preflight would hard-reject — same ordering as `tryPreflightSampleCoords`.
    static bool wouldSkipSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when coords are valid for trilinear interpolation (ordered slice corners).
    static bool isTrilinearSampleCoordsValid(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Trilinear-specific sample-coord preflight; rejects reversed slice corners.
    static bool tryPreflightTrilinearSampleCoords(const FroxelSampleCoords& coords,
                                                  TrilinearSampleRejectReason& outReason);
    /// True when slice corners or tz would be clamped before trilinear sampling.
    static bool wouldClampTrilinearSlice(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// Classify sample-coord reject — same ordering as `tryPreflightSampleCoords` (B5.11 deepen).
    /// Classify why sample-coord preflight would reject or warn (B5.11 deepen).
    /// Sample-coord preflight with optional reject-reason output (B5.11 deepen).
    /// True when slice corners or `tz` exceed grid bounds or lie outside [0, 1].
    static bool isTrilinearSliceOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
    /// True when slice interpolation would be clamped before trilinear sampling.
    /// Strict sample-coord validation; false on empty desc, hard OOB corners, or invalid weights.
    /// Classify why screen-depth → sample coords would reject.
                                                                 f32 screenY,
                                                                 f32 viewDepth,
    /// Non-mutating screen-depth → sample-coord preflight — returns true when mapping would proceed.
    static bool preflightScreenDepthToSampleCoords(f32 screenX,
                                                   const FroxelCameraDesc& camera,
    /// Non-mutating screen-depth → froxel-index preflight — returns true when mapping would proceed.
    static bool preflightScreenDepthToFroxelIndex(f32 screenX,
                                                  u32* outFroxelIndex = nullptr,
    /// Early-out when screen-depth mapping preflight would reject.
    static bool wouldSkipScreenDepthMapping(f32 screenX,
    /// Grid-only sample-coord preflight with optional reject-reason diagnostics.
    /// Classify why screen-depth mapping would reject — same ordering as `tryMapScreenDepthToSampleCoords`.
    /// Screen-depth mapping preflight with optional reject-reason diagnostics.
    static bool preflightScreenDepthMapping(f32 screenX,
    /// Classify screen-depth → sample-coords rejection — same ordering as `tryMapScreenDepthToSampleCoords`.
    /// Non-mutating screen-depth → sample-coords preflight — returns true when mapping would proceed.
    /// Classify sample-coord rejection — same ordering as `tryPreflightSampleCoords` (B5.11 deepen).
    /// Non-mutating sample-coord preflight — returns true when sampling would proceed (B5.11 deepen).
    static bool preflightFroxelSampleCoords(const FroxelSampleCoords& coords,
    /// Classify screen-depth → sample-coords rejection (B5.11 deepen).
    /// Non-mutating screen-depth preflight — returns true when mapping would proceed (B5.11 deepen).
    static bool preflightFroxelScreenDepth(f32 screenX,
    /// Non-mutating sample-coord preflight — returns true when sampling may proceed.
    static ScreenMappingRejectReason classifyScreenDepthMappingReject(f32 screenX,
    /// Non-mutating screen-depth mapping preflight — returns true when mapping may proceed.
    /// Early-out when screen-depth → sample-coords mapping would be rejected.
    /// Early-out when screen-depth → froxel index mapping would be rejected.
    static bool wouldSkipScreenDepthToFroxelIndex(f32 screenX,
    /// Grid-only sample-coord preflight — returns true when sampling would proceed.
    /// Classify screen-depth mapping rejection — same ordering as `tryMapScreenDepthToSampleCoords`.
    /// Non-mutating screen-depth mapping preflight — returns true when mapping would proceed.
    /// Early-out when screen-depth mapping would be rejected — same ordering as `tryMapScreenDepthToSampleCoords`.
    static bool wouldSkipScreenMapping(f32 screenX,
    static SampleCoordRejectReason classifySampleCoordsReject(const FroxelSampleCoords& coords,
    /// Non-mutating screen-depth → sample-coords preflight with optional output coords.
    /// Non-mutating screen-depth → froxel-index preflight with optional output index.
    /// Classify why screen-depth → froxel mapping would reject — same ordering as `tryMapScreenDepthToSampleCoords`.
    /// Classify screen-depth → sample-coords mapping — same ordering as `tryMapScreenDepthToSampleCoords`.
};

/// Why screen-depth → sample-coord mapping rejected the request (B5.11 deepen).
enum class SampleCoordRejectReason : u8 {
    None = 0,
    EmptyGrid,
    DepthOutOfRange,
};

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// Screen-depth → sample-coords with guard preflight and reject-reason diagnostics.
bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                     f32 screenY,
                                     f32 viewDepth,
                                     const FroxelGridDesc& desc,
                                     const FroxelCameraDesc& camera,
                                     FroxelSampleCoords& outCoords,
                                     SampleCoordRejectReason& outReason);

/// Why grid density validation rejected a froxel cache (B5.11 deepen).
enum class GridDensityRejectReason : u8 {
    EmptyDesc,
    DescMismatch,
    UndersizedStorage,
    DensityCountMismatch,
    NonFiniteDensity,
};

/// Human-readable label for density reject reasons (logging / tests).
const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason);

/// True when a grid-density reject reason would block validation (B5.11 deepen pass).
/// True when a grid-density reject reason would block validation (B5.11 deepen).
/// True when a grid-density reject reason blocks guarded validation (B5.11 deepen).
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
    EmptyStorage,
    IndexOutOfRange,
    SampleCoordsOutOfRange,
};

/// Why analytic froxel populate preflight rejected the request (B5.11 deepen).
enum class FroxelPopulateRejectReason : u8 {
    None = 0,
    EmptyGrid,
    ZeroDensity,
    ZeroMarchSteps,
    InvalidCamera,
    DescMismatch,
    EmptyStorage,
    IndexOutOfRange,
    ScreenMappingFailed,
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
    OutOfRange,

/// Human-readable label for sample-coord reject reasons (logging / tests).
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason);

/// Why analytic froxel populate was rejected (B5.11 deepen).
enum class FroxelPopulateRejectReason : u8 {
    None = 0,
    EmptyGrid,
/// Why analytic froxel populate preflight rejected or skipped non-zero fill (B5.11 deepen).
/// Why analytic froxel populate preflight rejected the request (B5.11 deepen).
enum class PopulateRejectReason : u8 {
    InvalidCamera,
    ZeroDensity,
    ZeroMarchSteps,
};

/// Human-readable label for populate reject reasons (logging / tests).
const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason);
const char* populateRejectReasonLabel(PopulateRejectReason reason);

/// Classify why analytic froxel populate would skip meaningful fill (B5.11 deepen).
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Populate preflight with optional reject-reason output (B5.11 deepen).
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Populate preflight with mandatory reject-reason output (B5.11 deepen).
bool tryPreflightFroxelPopulate(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params,
                                FroxelPopulateRejectReason& reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block analytic fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block analytic fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason blocks meaningful analytic fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful analytic fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block analytic fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block analytic fill (B5.11 deepen pass).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// True when a populate reject reason would block meaningful fill (B5.11 deepen).
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

/// Why a froxel density lookup preflight rejected the request (B5.11 deepen).
enum class DensityLookupRejectReason : u8 {
    None = 0,
    EmptyGrid,
    DescMismatch,
    EmptyStorage,
    IndexOutOfRange,
    ScreenMappingFailed,
    SampleCoordRejected,
    CoordOutOfRange,
};

/// Why a froxel trilinear density sample preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    None = 0,
    LookupFailed,
    InvalidSampleCoords,
};

/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason);

/// Human-readable label for density lookup reject reasons (logging / tests).
const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason);

/// Why coord-based froxel trilinear sampling preflight rejected the request (B5.11 deepen).
/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen).
enum class FroxelTrilinearSampleRejectReason : u8 {
    None = 0,
    EmptyGrid,
    NotSampleable,
    EmptyStorage,
    UndersizedStorage,
    DescMismatch,
    InvalidSampleCoords,
    InaccessibleGrid,
    OutOfBoundsCoords,
    ScreenMappingFailed,
/// Why froxel trilinear/bilinear density sampling preflight rejected the request (B5.11 deepen).
    NotAccessible,
/// Why trilinear density sample preflight rejected the request (B5.11 deepen).
    CoordsOutOfRange,
    InvalidWeights,
    HardOutOfBounds,
/// True when a density lookup reject reason would block lookup (B5.11 deepen pass).
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason);

/// Why froxel trilinear density sampling preflight rejected the request (B5.11 deepen pass).
    ClampableWeights,
/// True when a density lookup reject reason would block lookup (B5.11 deepen).

/// True when a density-lookup reject reason would block lookup (B5.11 deepen).

/// Why froxel camera validation rejected the request (B5.11 deepen).
enum class FroxelCameraRejectReason : u8 {
    InvalidPlanes,
    ZeroScreenDimensions,
};

/// Human-readable label for camera reject reasons (logging / tests).
const char* froxelCameraRejectReasonLabel(FroxelCameraRejectReason reason);

/// True when a camera reject reason would block froxel screen mapping (B5.11 deepen).
bool froxelCameraRejectReasonIsBlocking(FroxelCameraRejectReason reason);

/// True when near/far planes and screen dimensions are valid for froxel mapping.
bool isValidFroxelCamera(const FroxelCameraDesc& camera);

/// Diagnose why froxel camera validation would reject (B5.11 deepen).
bool tryValidateFroxelCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason);


/// Human-readable label for trilinear sample reject reasons (logging / tests).
const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason);

/// Classify density lookup reject — same ordering as `tryCanLookupAtIndex` (B5.11 deepen).
DensityLookupRejectReason classifyFroxelDensityLookupReject(const FroxelDensityGrid& grid,
                                                            const FroxelGridDesc& desc,
                                                            u32 index);
/// Classify sample-coord reject — same ordering as `tryPreflightSampleCoords` (B5.11 deepen).
SampleCoordRejectReason classifyFroxelSampleCoordReject(const FroxelSampleCoords& coords,
                                                        const FroxelGridDesc& desc);
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
/// Classify analytic populate reject — same ordering as `tryCanPopulateFromAnalyticFog` (B5.11 deepen).
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Classify screen-depth mapping reject — same ordering as `tryMapScreenDepthToSampleCoords` (B5.11 deepen).
ScreenMappingRejectReason classifyFroxelScreenMappingReject(f32 screenX,
                                                              f32 screenY,
                                                              f32 viewDepth,
                                                              const FroxelCameraDesc& camera);
/// Classify trilinear sample reject — same ordering as `tryCanSampleTrilinearAtCoords` (B5.11 deepen).
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelSampleCoords& coords);
enum class DensityTrilinearSampleRejectReason : u8 {

const char* densityTrilinearSampleRejectReasonLabel(DensityTrilinearSampleRejectReason reason);
/// Classify trilinear sample reject — same ordering as `tryCanTrilinearSampleAtCoords` (B5.11 deepen).
/// Classify why a density lookup at `index` would be rejected or clamped (B5.11 deepen).
/// Classify why a density lookup at tile/slice coords would be rejected or clamped (B5.11 deepen).
DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ);
/// True when a trilinear sample reject reason would block sampling (B5.11 deepen pass).
bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason);

/// True when a density-lookup reject reason would block lookup (B5.11 deepen pass).

/// True when a trilinear sample reject reason would block sampling (B5.11 deepen).






/// True when a density-lookup reject reason blocks guarded lookup (B5.11 deepen).





/// CPU froxel density interpolation helpers — mirrors CUDA trilinear sample stub.
namespace froxel_util {
f32 lerpDensity(f32 a, f32 b, f32 t);
/// True when density storage matches the clamped froxel count for `desc`.
bool gridMatchesDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// True when `desc` is non-empty, storage is allocated, and sizes match.
bool isDensityGridAccessible(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when the grid is inaccessible for index-based density lookup.
bool shouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Predict density lookup skip — same ordering as `classifyFroxelDensityLookupReject` (B5.11 deepen).
bool wouldSkipFroxelDensityLookup(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    DensityLookupRejectReason* reason = nullptr);
/// Early-out when density lookup would be rejected — same ordering as `tryCanLookupAtIndex`.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when coord density lookup would be rejected — same ordering as `tryCanLookupAtCoord`.
bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ);
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
/// True when `desc` has a non-zero clamped froxel count suitable for allocation/populate.
bool canAllocateFroxelGrid(const FroxelGridDesc& desc);
/// Early-out when analytic fog populate should be skipped for an empty desc.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc);
/// Early-out alias for `shouldSkipFroxelGrid` (B5.11 deepen).
bool wouldSkipFroxelGrid(const FroxelGridDesc& desc);
/// Early-out alias for `shouldSkipFroxelLookup` (B5.11 deepen).
bool wouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out alias for `shouldSkipFroxelMarch` (B5.11 deepen).
bool wouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Early-out alias for `shouldSkipFroxelPopulate` (B5.11 deepen).
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
/// Classify why density lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index);
/// Classify why coord density lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ);
/// Density lookup preflight with optional reject-reason diagnostics.
bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 index,
                            DensityLookupRejectReason* reason = nullptr);
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
/// Non-mutating index density lookup preflight — returns true when lookup would proceed.
/// Non-mutating coord density lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
/// Preflight guard before index-based density lookup; false on empty grid or desc mismatch.
bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Quick density-lookup preflight without index diagnostics.
bool preflightDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Density lookup preflight with index clamp warning.
bool tryPreflightDensityLookup(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               u32 index,
                               DensityLookupRejectReason& outReason);
/// Preflight guard before tile/slice coord density lookup; false on empty grid or desc mismatch.
/// Preflight guard before tile/slice coord lookup; false on empty grid or desc mismatch.
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
/// True when the froxel grid can participate in spatial density sampling.
bool canSampleFroxelGrid(const FroxelGridDesc& desc);
/// Minimum density storage entries for sampling; 0 when the grid is not sampleable.
u32 requiredFroxelCount(const FroxelGridDesc& desc);
/// True when `grid` storage covers every froxel in `desc`.
bool isDensitySizedForGrid(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Density-storage shortfall vs `requiredFroxelCount`; 0 when sized or the grid is not sampleable.
u32 densityEntriesMissing(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Diagnose why density-index preflight would reject; vacuously succeeds on accessible grids.
bool tryValidateDensityLookupIndex(const FroxelDensityGrid& grid,
                                   DensityLookupRejectReason& outReason);
/// Classify density lookup rejection at a flat index — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupRejectAtIndex(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
/// Classify density lookup rejection at tile/slice coords — same ordering as `tryCanLookupAtCoord`.
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ);
/// Classify density-lookup rejection — same ordering as `tryCanLookupAtIndex` (B5.11 deepen).
/// Classify coord density-lookup rejection — same ordering as `tryCanLookupAtCoord` (B5.11 deepen).
DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
/// Non-mutating index lookup preflight — returns true when lookup would proceed (B5.11 deepen).
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
/// Non-mutating coord lookup preflight — returns true when lookup would proceed (B5.11 deepen).
/// Classify why index-based density lookup would reject — same ordering as `tryCanLookupAtIndex`.
/// Classify why coord-based density lookup would reject — same ordering as `tryCanLookupAtCoord`.
/// Non-mutating index lookup preflight — returns true when lookup would proceed.
/// Non-mutating coord lookup preflight — returns true when lookup would proceed.
/// Diagnose why lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         DensityLookupRejectReason& outReason);
/// Classify why index-based density lookup would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index);
/// Classify why coord-based density lookup would reject — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ);
/// Non-mutating index-based density lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason = nullptr);
/// Non-mutating coord-based density lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason = nullptr);
/// Diagnose why coord lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
/// Early-out when density lookup would be rejected — same ordering as `tryCanLookupAtIndex`.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when index-based density lookup would be rejected; OOB indices that clamp are not skipped.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Early-out when coord-based density lookup would be rejected; OOB coords that clamp are not skipped.
bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason);
/// Preflight guard before tile/slice coord lookup; false on empty grid or desc mismatch.
bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      u32 sliceZ);
/// Diagnose why coord lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason);
/// Density lookup preflight with mandatory reject-reason output (B5.11 deepen).
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 index,
                                   DensityLookupRejectReason& outReason);
/// Density coord lookup preflight with mandatory reject-reason output (B5.11 deepen).
/// Density lookup preflight at index with optional reject-reason output (B5.11 deepen).
                                   DensityLookupRejectReason* reason = nullptr);
/// Density lookup preflight at index with mandatory reject-reason output (B5.11 deepen).
bool tryPreflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                      DensityLookupRejectReason& reason);
/// Density lookup preflight at coords with optional reject-reason output (B5.11 deepen).
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
/// Early-out alias for `shouldSkipFroxelLookup` (B5.11 deepen).
bool shouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Diagnose why coord lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
/// Diagnose why tile/slice coord lookup preflight would reject; vacuously succeeds on accessible grids.
/// Density lookup preflight with optional reject-reason diagnostics (B5.11 deepen).
bool preflightDensityLookup(const FroxelDensityGrid& grid,
bool tryPreflightDensityLookup(const FroxelDensityGrid& grid,
/// Early-out when index-based density lookup should be skipped (B5.11 deepen).
bool shouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// True when the grid is accessible and `index` lies in the valid froxel range (B5.11 deepen).
bool densityLookupReady(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Strict density lookup preflight; false when lookup is inaccessible or index is out of range.
bool tryPreflightStrictDensityLookupAtIndex(const FroxelDensityGrid& grid,
/// Strict density lookup preflight at tile/slice coords; false when inaccessible or coords are OOB.
bool tryPreflightStrictDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// True when strict density lookup preflight would reject the request.
bool wouldRejectDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// True when strict coord density lookup preflight would reject the request.
bool wouldRejectDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                     u32 sliceZ);
/// True when index-based density lookup should be skipped (inaccessible grid).
bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// True when coord-based density lookup should be skipped (inaccessible grid).
bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// Classify why density lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                    u32 index);
/// Classify why coord density lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
/// Non-mutating density lookup preflight — returns true when lookup may proceed.
/// Non-mutating coord density lookup preflight — returns true when lookup may proceed.
/// Classify why index lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
/// Classify why coord lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
/// Non-mutating index lookup preflight — returns true when lookup would proceed.
/// Non-mutating coord lookup preflight — returns true when lookup would proceed.
/// Classify index-based density lookup — same ordering as `tryCanLookupAtIndex`.
/// Classify tile/slice coord density lookup — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
/// Non-mutating index-based density lookup preflight — returns true when lookup would proceed.
/// Non-mutating coord-based density lookup preflight — returns true when lookup would proceed.
/// Early-out when density lookup would be rejected — same ordering as `tryCanLookupAtIndex`.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when index-based density lookup would be rejected; OOB indices that clamp are not skipped.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Early-out when coord-based density lookup would be rejected; OOB coords that clamp are not skipped.
/// Diagnose why density sampling preflight would reject before index/coord lookup.
bool tryCanLookupForDensitySample(const FroxelDensityGrid& grid,
/// Density lookup preflight at coords with mandatory reject-reason output (B5.11 deepen).
bool tryPreflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// True when `index` lies within the valid froxel range for a non-empty grid.
bool isDensityLookupIndexInRange(u32 index, const FroxelGridDesc& desc);
/// Strict index lookup preflight; false on empty grid, desc mismatch, or OOB index.
bool canLookupAtIndexStrict(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Strict index lookup preflight with reject-reason diagnostics.
bool tryCanLookupAtIndexStrict(const FroxelDensityGrid& grid,
/// Strict coord lookup preflight; false on empty grid, desc mismatch, or OOB coords.
bool canLookupAtCoordStrict(const FroxelDensityGrid& grid,
/// Strict coord lookup preflight with reject-reason diagnostics.
bool tryCanLookupAtCoordStrict(const FroxelDensityGrid& grid,
/// Coord density lookup preflight with optional reject-reason diagnostics (B5.11 deepen).
/// True when a lookup at `index` would clamp into the valid froxel range.
bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc);
/// True when a lookup at tile/slice coords would clamp into the valid froxel range.
bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
/// Combined empty-desc + grid accessibility preflight before density lookup/sample.
bool tryPreflightDensityGridAccess(const FroxelDensityGrid& grid,
                                   GridDensityRejectReason& outReason);
/// Diagnose why a flat froxel index is invalid; vacuously succeeds on in-range indices.
bool tryValidateFroxelIndex(u32 index, const FroxelGridDesc& desc, DensityLookupRejectReason& outReason);
/// Early-out when analytic froxel populate should be skipped.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params);
/// Preflight guard before analytic froxel populate; false on empty grid or disabled fog params.
bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params);
/// Diagnose why analytic populate preflight would reject.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason);
/// Preflight guard before tile/slice coord density lookup; false on inaccessible grid.
bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      u32 sliceZ);
/// Diagnose coord lookup preflight; vacuously succeeds on accessible grids with clamp warnings.
/// True when tile/slice coords would clamp before density lookup.
/// True when tile/slice coords exceed grid bounds and would clamp before lookup.
/// True when tile/slice coords exceed grid bounds before clamping.
/// True when tile/slice coords exceed grid bounds (would be clamped before lookup).
bool wouldClampCoordLookup(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
/// True when tile/slice coords would clamp before a density lookup.
/// True when index-based density lookup would hard-reject (inaccessible grid/desc).
bool wouldRejectDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// True when coord-based density lookup would hard-reject (inaccessible grid/desc).
bool wouldRejectDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     u32 tileX,
                                     u32 tileY,
/// Diagnose density lookup preflight at a flat index; false on hard reject.
bool tryPreflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                      u32 index,
                                      DensityLookupRejectReason& outReason);
/// Diagnose density lookup preflight at tile/slice coords; false on hard reject.
bool tryPreflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                      u32 sliceZ,
/// Early-out when index-based density lookup would be rejected — same ordering as `tryCanLookupAtIndex`.
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Early-out when lookup at `index` would be rejected — OOB indices that clamp still return false.
bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   u32 index);
/// Early-out when lookup at tile/slice coords would be rejected — OOB coords that clamp still return false.
bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// Classify why index lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupIndexReject(const FroxelDensityGrid& grid,
/// Classify why coord lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
/// Non-mutating index lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   DensityLookupRejectReason* reason = nullptr);
/// Non-mutating coord lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// Early-out when index lookup preflight would reject.
bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index);
/// Early-out when coord lookup preflight would reject.
/// Preflight guard before coord-based density sampling; false on inaccessible grid or invalid coords.
bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& coords);
/// Classify trilinear sample rejection — same ordering as `tryCanSampleAtTrilinear`.
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords);
/// Diagnose why trilinear sample preflight would reject.
bool tryCanSampleAtTrilinear(const FroxelDensityGrid& grid,
                             const FroxelSampleCoords& coords,
                             FroxelTrilinearSampleRejectReason& outReason);
/// Non-mutating trilinear sample preflight with optional density output.
bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    f32* outDensity = nullptr,
                                    FroxelTrilinearSampleRejectReason* outReason = nullptr);
/// Classify coord-based trilinear sample rejection (B5.11 deepen).
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed (B5.11 deepen).
                                    FroxelTrilinearSampleRejectReason* reason = nullptr);
/// Diagnose why coord-based sample preflight would reject.
bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords,
                          SampleCoordRejectReason& outReason);
/// Preflight guard before bilinear density sampling; false on inaccessible grid or hard OOB coords.
bool canBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords);
/// Diagnose why bilinear sample preflight would reject; warns on clampable weights.
bool tryCanBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelSampleCoords& coords,
                                  FroxelBilinearSampleRejectReason& outReason);
/// Early-out when bilinear density sampling would be rejected — same ordering as `tryCanBilinearSampleAtCoords`.
bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords);
/// Preflight guard before trilinear density sampling; false on inaccessible grid or hard OOB coords.
bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              FroxelTrilinearSampleRejectReason* reason = nullptr);
                                const FroxelGridDesc& desc,
                                const FroxelSampleCoords& coords);
/// Classify why trilinear density sampling would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelSampleCoords& coords,
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords);
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
                                    FroxelTrilinearSampleRejectReason* reason = nullptr);
/// Diagnose why trilinear sample preflight would reject; warns on clampable weights.
bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason);
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords);
/// Non-mutating trilinear sample preflight — returns true when sampling may proceed.
/// Classify trilinear density sample preflight — same ordering as `tryCanTrilinearSampleAtCoords`.
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason = nullptr);
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
/// Preflight guard before coord-based density sampling; false when lookup or coords would be rejected.
                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason);
/// True when lookup passes and sample coords are ordered with in-range weights.
bool canSampleValidCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords);
/// Classify why a guarded froxel sample would skip — same ordering as `wouldSkipFroxelSample`.
SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
/// Predict whether guarded bilinear/trilinear sampling would bail before interpolation.
bool wouldSkipFroxelSample(const FroxelDensityGrid& grid,
                         const FroxelSampleCoords& coords,
                         SampleCoordRejectReason* outReason = nullptr);
/// True when tile/slice corners and interpolation weights are valid for sampling.
bool areSampleCoordsReady(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
/// True when lookup succeeds and sample coords are valid without clamping.
bool canSampleAtCoordsStrict(const FroxelDensityGrid& grid,
/// Diagnose strict sample-coord preflight; false when lookup fails or coords need clamping.
bool tryCanSampleAtCoordsStrict(const FroxelDensityGrid& grid,
                                SampleCoordRejectReason& outReason);
/// Diagnose why coord-based trilinear sample preflight would reject (unified reject reasons).
bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
/// Diagnose why trilinear sample preflight would reject.
bool tryCanSampleDensityTrilinear(const FroxelDensityGrid& grid,
/// Diagnose why trilinear density sampling preflight would reject.
bool tryCanSampleTrilinear(const FroxelDensityGrid& grid,
bool canSampleDensityTrilinear(const FroxelDensityGrid& grid,
/// Trilinear/bilinear sample preflight with dedicated reject-reason diagnostics.
bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
/// Predict trilinear sample skip — same ordering as `classifyFroxelTrilinearSampleReject` (B5.11 deepen).
bool wouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
/// Preflight guard before trilinear density sampling.
bool canSampleTrilinear(const FroxelDensityGrid& grid,
/// True when trilinear sampling would clamp coords or weights on an accessible grid.
bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
/// Trilinear sample preflight without reject-reason diagnostics.
bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
/// Non-mutating preflight for coord-based density sampling without reject-reason diagnostics.
bool preflightSampleAtCoords(const FroxelDensityGrid& grid,
/// Non-mutating preflight for coord-based density sampling with reject-reason diagnostics.
/// True when interpolation weights or corner indices would be clamped before coord-based sampling.
bool wouldClampSampleAtCoords(const FroxelDensityGrid& grid,
/// Non-mutating preflight for trilinear density sampling without reject-reason diagnostics.
/// Non-mutating preflight for trilinear density sampling with reject-reason diagnostics.
/// Non-mutating preflight for index-based density lookup without reject-reason diagnostics.
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   u32 index);
/// Non-mutating preflight for index-based density lookup with reject-reason diagnostics.
/// Non-mutating preflight for tile/slice coord density lookup without reject-reason diagnostics.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
/// Non-mutating preflight for tile/slice coord density lookup with reject-reason diagnostics.
/// Diagnose why trilinear sample preflight would reject; vacuously succeeds with clamp warnings.
                                   DensityTrilinearSampleRejectReason& outReason);
/// True when trilinear sample would clamp coords or weights before sampling.
bool wouldClampDensityTrilinearSample(const FroxelDensityGrid& grid,
/// Trilinear sample preflight combining grid access and trilinear coord validation.
                                   TrilinearSampleRejectReason& outReason);
/// True when trilinear sampling would clamp sample coords before interpolation.
/// Preflight guard before trilinear density sampling; false on inaccessible grid or invalid coords.
/// True when trilinear sampling would clamp lookup indices or interpolation weights.
bool wouldClampTrilinearSampleCoords(const FroxelDensityGrid& grid,
/// True when sample coords would be hard-rejected (not merely clamped) before sampling.
bool wouldRejectSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
/// Early-out when trilinear density sampling should be skipped for an inaccessible grid.
bool shouldSkipFroxelTrilinear(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// True when density lookup or sample coords would clamp before trilinear sampling.
/// Early-out when trilinear density sampling should be skipped (B5.11 deepen).
bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// True when interpolation weights or corner indices would be clamped before trilinear sampling.
bool wouldClampTrilinearSample(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
/// Diagnose why trilinear sample preflight would reject (B5.11 deepen).
/// Classify why a trilinear density sample would be rejected or warned (B5.11 deepen).
SampleCoordRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
/// Trilinear sample preflight with optional reject-reason output (B5.11 deepen).
                              SampleCoordRejectReason* reason = nullptr);
/// Trilinear sample preflight with mandatory reject-reason output (B5.11 deepen).
                                 SampleCoordRejectReason& reason);
/// Early-out when trilinear sample preflight would reject (B5.11 deepen).
bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid,
/// Early-out when trilinear sampling should be skipped for inaccessible storage or hard OOB coords.
bool shouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
/// Combined grid + sample-coord trilinear preflight with reject-reason diagnostics.
/// Early-out when trilinear density sampling should be skipped (hard preflight rejection).
/// True when lookup or sample coords would clamp before trilinear sampling.
bool wouldClampTrilinearDensitySample(const FroxelDensityGrid& grid,
/// Grid + sample-coord preflight for trilinear sampling without reject-reason diagnostics.
bool canPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
/// Grid + sample-coord preflight for trilinear sampling with reject-reason diagnostics.
bool tryPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
/// Sample-coord preflight with optional reject-reason diagnostics (B5.11 deepen).
/// Preflight guard before coord-based trilinear sampling.
bool canSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
/// Diagnose why coord-based trilinear sample preflight would reject.
/// Early-out when trilinear sampling would be rejected — same ordering as `tryCanSampleTrilinearAtCoords`.
/// Classify why coord-based sample preflight would reject — same ordering as `tryCanSampleAtCoords`.
SampleCoordRejectReason classifyDensitySampleCoordReject(const FroxelDensityGrid& grid,
/// Non-mutating coord-based sample preflight — returns true when sampling would proceed.
bool preflightDensitySampleAtCoords(const FroxelDensityGrid& grid,
/// Early-out when coord-based sample preflight would reject.
bool wouldSkipDensitySampleAtCoords(const FroxelDensityGrid& grid,
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSampleAtCoords`.
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,
                                     FroxelTrilinearSampleRejectReason* reason = nullptr);
/// Early-out when bilinear density sampling would be rejected — same ordering as `tryCanSampleAtCoords`.
bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
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
/// Non-zero froxel count using the clamped froxel count derived from `desc`; returns 0 on mismatch.
u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Count froxels with density at or below `epsilon`; returns 0 when the grid is empty.
u32 countEmptyFroxels(const FroxelDensityGrid& grid, f32 epsilon = 1e-6f);
/// Read density at a clamped flat froxel index; returns 0 when access is denied.
/// Non-zero froxel count using the clamped froxel count derived from `desc`; returns 0 on mismatch.
u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Empty froxel count using the clamped froxel count derived from `desc`; returns froxelCount on mismatch.
u32 countEmptyFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// True when grid is accessible but all froxels are at or below `epsilon`.
bool isDensityGridFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// True when grid is accessible but uniformly below `epsilon`.
bool isDensityFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Validate density counts against the clamped froxel count derived from `desc`.
/// Validate density partition counts against the clamped froxel count derived from `desc`.
bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Read density at a clamped flat froxel index; returns 0 when grid/desc mismatch or empty.
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
/// Diagnose density validation against `desc`; vacuously succeeds on empty grids.
bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
/// Diagnose density validation against the clamped froxel count derived from `desc`.
/// Diagnose density validation against `desc`; vacuously succeeds when `desc` is empty.
/// Diagnose grid density validation against clamped `desc`; vacuously succeeds when `desc` is empty.
/// Classify why grid-density validation would reject — same ordering as `tryValidateGridDensity`.
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
                          GridDensityRejectReason* reason = nullptr);
/// Classify the first density invariant that fails — same ordering as `tryValidateGridDensity`.
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
/// Diagnose density validation against `desc`; rejects non-empty storage on empty desc.
/// Diagnose why sample coords fail bounds checks; vacuously succeeds when in bounds.
                             SampleCoordBoundsRejectReason& outReason);
/// Validate density counts against the clamped froxel count derived from `desc`.
bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Non-zero froxel count using the clamped froxel count derived from `desc`; returns 0 on mismatch.
u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Empty froxel count using the clamped froxel count derived from `desc`; returns froxelCount on mismatch.
u32 countEmptyFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// True when grid is accessible but every froxel is at or below `epsilon`.
bool isDensityFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon = 1e-6f);
/// Classify grid-density rejection — same ordering as `tryValidateGridDensity`.
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
/// Non-mutating grid-density preflight with optional reject-reason diagnostics.
bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* outReason = nullptr,
/// Classify why grid density validation would reject — same ordering as `tryValidateGridDensity`.
/// Non-mutating grid density preflight — returns true when validation would succeed.
                          GridDensityRejectReason* reason = nullptr,
/// Early-out when grid density validation preflight would reject.
bool wouldSkipGridDensityValidation(const FroxelDensityGrid& grid,
/// Grid density preflight with optional reject-reason diagnostics.
/// Early-out when grid density validation would fail (B5.11 deepen).
/// True when non-zero + empty froxel counts partition storage for `desc`.
/// Non-zero froxel count for `desc`; returns 0 on desc mismatch.
/// Empty froxel count for `desc`; returns clamped froxel count on desc mismatch.
/// True when grid is accessible but uniformly at or below `epsilon`.
/// Classify why grid-density validation would reject — same ordering as `tryValidateGridDensity`.
/// Non-mutating grid-density preflight — returns true when validation would proceed.
/// Classify why index lookup preflight would reject — same ordering as `tryCanLookupAtIndex`.
DensityLookupRejectReason classifyDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                                       u32 index);
/// Classify why coord lookup preflight would reject — same ordering as `tryCanLookupAtCoord`.
DensityLookupRejectReason classifyDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                                       u32 tileX,
                                                       u32 tileY,
                                                       u32 sliceZ);
/// Non-mutating index lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                     u32 index,
                                     DensityLookupRejectReason* reason = nullptr);
/// Non-mutating coord lookup preflight — returns true when lookup would proceed.
bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                     u32 sliceZ,
/// Classify why analytic populate would reject — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Non-mutating populate preflight — returns true when analytic fill would proceed.
bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                      const VolumetricFogParams& params,
                                      FroxelPopulateRejectReason* reason = nullptr);
/// Classify grid-density rejection — may report `EmptyDesc` or `NonFiniteDensity` (B5.11 deepen).
/// Non-mutating grid-density preflight — returns true when validation would pass (B5.11 deepen).
/// True when `value` is finite and non-negative (B5.11 deepen).
bool isFiniteDensityValue(f32 value);
/// Clamp non-finite or negative density to zero (B5.11 deepen).
f32 sanitizeDensityValue(f32 value);
/// Count froxels with non-finite density; returns 0 when storage is empty (B5.11 deepen).
u32 countNonFiniteDensities(const FroxelDensityGrid& grid);
/// True when at least one froxel holds a non-finite density value (B5.11 deepen).
bool hasNonFiniteDensity(const FroxelDensityGrid& grid);
/// Non-mutating grid density preflight — returns true when validation may proceed.
/// Diagnose density validation against `desc`; vacuously succeeds when `desc` is empty.
/// Early-out when grid density validation would be rejected — same ordering as `tryValidateGridDensity`.
/// Non-mutating grid density preflight — returns true when validation would pass.
/// Classify grid density validation — same ordering as `tryValidateGridDensity`.
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
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value);
/// Write density with guard preflight and reject-reason diagnostics.
/// Write density with guard preflight and lookup reject-reason diagnostics.
/// Read density with guard preflight and reject-reason diagnostics.
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason);
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value,
                            DensityLookupRejectReason& outReason);
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
/// Read density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
/// Read density at clamped tile/slice coords with guard preflight and lookup reject-reason diagnostics.
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
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value);
/// Write density at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
/// Write density at clamped tile/slice coords with guard preflight and lookup reject-reason diagnostics.
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value,
                            DensityLookupRejectReason& outReason);
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
                              DensityLookupRejectReason& outLookupReason,
                              SampleCoordRejectReason& outCoordReason);
/// Bilinear sample with guard preflight and bilinear-specific reject-reason diagnostics.
bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason);
f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelSampleCoords& coords);
/// Trilinear sample preflight with optional reject-reason diagnostics (B5.11 deepen).
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              SampleCoordRejectReason* reason = nullptr);
/// Trilinear sample preflight with mandatory reject-reason output (B5.11 deepen).
bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 const FroxelSampleCoords& coords,
                                 SampleCoordRejectReason& outReason);
/// Early-out when trilinear density sampling should be skipped (B5.11 deepen).
bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords);
/// True when trilinear sampling would clamp lookup indices or interpolation weights (B5.11 deepen).
bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
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
/// Preflight guard before froxel trilinear density sampling (B5.11 deepen).
bool canPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
/// Diagnose why trilinear density sample preflight would reject (B5.11 deepen).
bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 const FroxelSampleCoords& coords,
/// Early-out when trilinear density sampling should be skipped (B5.11 deepen).
bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Trilinear sample with combined trilinear preflight reject-reason diagnostics.
bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               f32& outDensity,
/// Trilinear sample with dedicated trilinear reject-reason diagnostics.
/// Trilinear sample with guard preflight and trilinear reject-reason diagnostics.
/// Trilinear sample with trilinear-specific reject-reason diagnostics.
/// Early-out when trilinear density sampling should be skipped — same ordering as `tryPreflightTrilinearSample`.
bool wouldSkipTrilinearSample(const FroxelDensityGrid& grid,
/// Preflight guard before trilinear density sampling.
/// Diagnose why trilinear sample preflight would reject; warns on clampable weights.
/// Trilinear sample with unified trilinear preflight and reject-reason diagnostics.
                               DensityTrilinearSampleRejectReason& outReason);
                               TrilinearSampleRejectReason& outReason);
/// Trilinear sample with trilinear-specific guard preflight and reject-reason diagnostics.
/// Trilinear sample with trilinear-specific reject-reason diagnostics (B5.11 deepen).
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightTrilinearDensitySample(const FroxelDensityGrid& grid,
                                     SampleCoordRejectReason* reason = nullptr);
/// Early-out when trilinear sample preflight would reject.
bool wouldSkipTrilinearDensitySample(const FroxelDensityGrid& grid,
/// Diagnose why trilinear sample preflight would reject.
bool tryCanTrilinearSample(const FroxelDensityGrid& grid,
/// Classify why trilinear sample preflight would reject — same ordering as `tryCanTrilinearSample`.
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
/// Trilinear sample preflight with optional reject-reason diagnostics.
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              FroxelTrilinearSampleRejectReason* reason = nullptr);
/// Early-out when trilinear sample preflight would reject (B5.11 deepen).
/// Preflight guard before trilinear density sampling; false on inaccessible grid or hard OOB coords.
bool canSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
/// Diagnose trilinear sample preflight; false on inaccessible grid or hard OOB coords.
bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
                                   SampleCoordRejectReason& outReason);
/// True when trilinear sampling would clamp coords or weights before interpolation.
bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
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
/// Screen-space sample with lookup and sample-coord reject-reason diagnostics.
/// Screen-space sample with screen-mapping reject-reason diagnostics.
/// Screen-space sample with combined trilinear preflight reject-reason diagnostics.
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
                              DensityLookupRejectReason& outLookupReason,
                              SampleCoordRejectReason& outCoordReason);
                              SampleCoordRejectReason& outSampleReason);
                              DensityLookupRejectReason& outReason,
/// Early-out when analytic froxel populate should be skipped for an empty desc or disabled params.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params);
/// Diagnose why analytic froxel populate would be rejected.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason);
                              ScreenMappingRejectReason& outMapReason,
                              DensityLookupRejectReason& outLookupReason);
/// Early-out when analytic froxel populate would skip the density fill loop.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params);
/// Preflight guard before analytic fog populate; false when populate would early-out after allocate.
bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Diagnose why populate preflight would reject.
                                   PopulateRejectReason& outReason);
/// Populate with guard preflight; always allocates like `populateFromAnalyticFog`.
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
/// True when a populated grid matches desc and holds non-zero density after a successful populate.
bool validatePopulatedDensity(const FroxelDensityGrid& grid,
                            f32 epsilon = 1e-6f);
                              ScreenMappingRejectReason& outScreenReason);
/// Screen-space sample with guard preflight and screen/sample reject-reason diagnostics.
bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              ScreenMappingRejectReason& outMapReason);
/// Screen-space sample with screen-mapping and sample-coord reject-reason diagnostics.
/// Screen-space sample with mapping and sample-coord reject-reason diagnostics.
/// Screen-space sample with trilinear preflight reject-reason diagnostics.
/// Screen-space sample with guard preflight and screen-mapping + sample-coord reject-reason diagnostics.
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              ScreenMappingRejectReason& outScreenReason,
                              FroxelTrilinearSampleRejectReason& outReason);
/// Screen-depth → density lookup with lookup reject-reason diagnostics.
bool tryLookupDensityFromScreen(const FroxelDensityGrid& grid,
/// Non-mutating screen-space density sample preflight — returns true when sampling would proceed.
bool preflightScreenDensitySample(const FroxelDensityGrid& grid,
                                  ScreenMappingRejectReason* reason = nullptr);
/// Early-out when screen-space density sample preflight would reject.
bool wouldSkipScreenDensitySample(const FroxelDensityGrid& grid,
/// Classify why screen-space density sampling would reject.
ScreenMappingRejectReason classifyScreenDensitySampleReject(const FroxelDensityGrid& grid,
                                                            f32 viewDepth);
/// Screen-space density sample preflight with optional reject-reason diagnostics.
/// Early-out when screen-space density sampling would reject (B5.11 deepen).
                                  f32 viewDepth);
void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params);
/// Early-out when populate would allocate but skip the analytic fill loop.
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params);
/// Alias for `!canPopulateFromAnalyticFog` (B5.11 deepen).
/// True when analytic populate would write non-zero froxel density for a valid camera.
bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params);
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Non-mutating populate preflight — returns true when fill would proceed.
/// Non-mutating populate preflight — returns true when analytic fill would proceed.
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Diagnose why analytic populate would skip meaningful fill.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason);
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
/// Non-mutating populate preflight — returns true when meaningful fill may proceed.
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
/// Classify analytic populate preflight — same ordering as `tryCanPopulateFromAnalyticFog`.
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Non-mutating populate preflight — returns true when analytic fill would proceed.
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Early-out when analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params);
/// Predict populate skip — same ordering as `classifyFroxelPopulateReject` (B5.11 deepen).
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Diagnose why populate would skip meaningful fill; returns true when fill would be skipped.
bool tryShouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                                 FroxelPopulateRejectReason& outReason);
/// Early-out when analytic populate would be rejected — same ordering as `tryCanPopulateFromAnalyticFog`.
bool wouldSkipAnalyticPopulate(const FroxelGridDesc& desc,
/// True when populate would allocate storage but skip the analytic fill loop.
bool wouldPopulateAllocateOnly(const FroxelGridDesc& desc,
/// Populate skip preflight with reject-reason diagnostics.
/// True when populate would allocate a froxel grid but skip the analytic fill loop.
bool wouldPopulateAllocateWithoutFill(const FroxelGridDesc& desc,
/// True when the clamped froxel desc can allocate storage for populate.
bool canAllocateFroxelGridForPopulate(const FroxelGridDesc& desc);
/// Grid-only populate allocation preflight; false when the clamped desc is empty.
bool tryPreflightPopulateAllocation(const FroxelGridDesc& desc, FroxelPopulateRejectReason& outReason);
/// Early-out when analytic populate preflight would reject meaningful fill.
/// True when analytic populate would write non-zero froxel density for a valid camera.
bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
/// Non-mutating analytic populate preflight — returns true when meaningful fill would proceed.
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
FroxelPopulateRejectReason classifyPopulateReject(const FroxelGridDesc& desc,
                                                  const FroxelCameraDesc& camera,
                                                  const VolumetricFogParams& params);
/// Non-mutating populate preflight — returns true when meaningful fill would proceed.
/// Non-mutating populate preflight — returns true when analytic fill would proceed.
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Guarded populate — always mirrors `populateFromAnalyticFog`; returns false when preflight rejects fill.
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params);
/// Grid-only populate preflight without reject-reason diagnostics.
bool canPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// True when `desc` has a non-zero clamped froxel count (populate preflight wrapper).
bool canPopulateFromAnalyticFogForDesc(const FroxelGridDesc& desc);
/// Classify analytic populate rejection — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Diagnose why analytic populate would skip meaningful fill.
bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   FroxelPopulateRejectReason& outReason);
/// Early-out when analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
/// Analytic populate preflight with optional reject-reason diagnostics (B5.11 deepen).
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params);
/// Analytic populate preflight with optional reject-reason diagnostics.
/// Classify populate rejection — same ordering as `tryCanPopulateFromAnalyticFog` (B5.11 deepen).
/// Non-mutating populate preflight — returns true when fill would proceed (B5.11 deepen).
bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason = nullptr);
/// Analytic populate preflight with mandatory reject-reason output (B5.11 deepen).
bool tryPreflightFroxelPopulate(const FroxelGridDesc& desc,
                                FroxelPopulateRejectReason& outReason);
/// True when analytic populate would write non-zero froxel density (B5.11 deepen).
bool froxelPopulateReady(const FroxelGridDesc& desc,
                         const VolumetricFogParams& params);
/// Non-mutating populate preflight; optional reject-reason output.
bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                      FroxelPopulateRejectReason* outReason = nullptr);
/// Non-mutating preflight for analytic populate without reject-reason diagnostics.
/// Non-mutating preflight for analytic populate with reject-reason diagnostics.
/// True when populate would reallocate density storage to match `desc`.
bool needsPopulateReallocate(const FroxelDensityGrid& grid, const FroxelGridDesc& desc);
/// Populate preflight including output grid compatibility check.
bool tryPreflightPopulate(const FroxelGridDesc& desc,
                          const FroxelDensityGrid& grid,
/// Diagnose populate preflight without mutating density storage.
bool tryPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Populate preflight with mandatory reject-reason output (B5.11 deepen).
/// Validate populate output against preflight expectations; vacuously succeeds when fill was skipped.
bool tryValidatePopulateResult(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
/// Classify why analytic populate would skip meaningful fill — same ordering as `tryCanPopulateFromAnalyticFog`.
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
/// Non-mutating populate preflight — returns true when fill would proceed.
/// Early-out when populate preflight would skip meaningful fill.
bool wouldSkipPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Guarded populate — always mirrors `populateFromAnalyticFog`; returns false when preflight rejects fill.
/// Populate with guard preflight; returns false when `canPopulateFroxelGrid` would reject the request.
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
/// Guarded populate with reject-reason diagnostics.
/// Populate with preflight guards; returns false without modifying `grid` when populate would be skipped.
/// Populate with guard preflight; returns false when `tryCanPopulateFromAnalyticFog` would reject.
/// Populate with guard preflight and reject-reason diagnostics.
                                FroxelGridRejectReason& outGridReason,
                                FroxelCameraRejectReason& outCameraReason);
/// True when `camera` has a positive near plane and far exceeds near.
bool isValidPopulateCamera(const FroxelCameraDesc& camera);
/// Early-out when analytic populate would not write non-zero froxel density.
/// True when analytic populate would fill froxels with non-zero density.
/// Diagnose why non-zero analytic populate would be skipped; false on empty grid or invalid camera.
/// Preflight analytic populate without mutation — same semantics as `tryCanPopulateFromAnalyticFog`.
bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
/// Populate with guard preflight; returns false when empty grid or invalid camera would be rejected.
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params);
bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
/// Guarded populate with reject-reason diagnostics; always mirrors `populateFromAnalyticFog`.
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params,
                                FroxelPopulateRejectReason& outReason);
/// Diagnose density validation against the clamped froxel count derived from `desc`.
bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon = 1e-6f);
/// Guarded populate with reject-reason diagnostics.
/// True when post-populate storage matches `desc` and density invariants hold for the fill path taken.
bool validatePopulateResult(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            const VolumetricFogParams& params,
/// Non-mutating trilinear sample preflight — returns true when sampling would proceed.
bool preflightTrilinearDensitySample(const FroxelDensityGrid& grid,
                                     const FroxelSampleCoords& coords,
                                     SampleCoordRejectReason* reason = nullptr);
/// Non-mutating screen-space density sample preflight — returns true when sampling would proceed.
bool preflightDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              ScreenMappingRejectReason* reason = nullptr);
} // namespace froxel_util

/// CPU stub — exponential height falloff density sample (P5 acceptance reference).
f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos);

/// Records logical volumetric fog work for the frame; returns false when disabled.
bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats);

/// Render-graph hook — inserts the volumetric fog CUDA pass after screen-space AO.
void resetVolumetricFogPassGraphStorage();
void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count);

} // namespace fuse::renderer
