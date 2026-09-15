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
    bool isEmpty() const { return density.empty(); }
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

/// Froxel grid indexing helpers — mirrors clustered light layout (B5.4).
struct FroxelGridLayout {
    static u32 froxelIndex(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc);
    static void decodeFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& tileX, u32& tileY, u32& sliceZ);
    static u32 clampFroxelIndex(u32 index, const FroxelGridDesc& desc);
    static u32 clampTileX(u32 tileX, const FroxelGridDesc& desc);
    static u32 clampTileY(u32 tileY, const FroxelGridDesc& desc);
    static u32 clampSliceZ(u32 sliceZ, const FroxelGridDesc& desc);
    static bool mapScreenDepthToSampleCoords(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const FroxelGridDesc& desc,
                                             const FroxelCameraDesc& camera,
                                             FroxelSampleCoords& outCoords);
    /// Screen-depth → linear froxel index (mirrors clustered `mapScreenDepthToClusterIndex`).
    static bool mapScreenDepthToFroxelIndex(f32 screenX,
                                            f32 screenY,
                                            f32 viewDepth,
                                            const FroxelGridDesc& desc,
                                            const FroxelCameraDesc& camera,
                                            u32& outFroxelIndex);
};

/// CPU froxel density interpolation helpers — mirrors CUDA trilinear sample stub.
namespace froxel_util {
f32 lerpDensity(f32 a, f32 b, f32 t);
f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords);
f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords);
/// Screen-space trilinear density sample; returns 0 when mapping fails or grid is empty.
f32 sampleDensityAtScreen(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelCameraDesc& camera,
                          f32 screenX,
                          f32 screenY,
                          f32 viewDepth);
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
