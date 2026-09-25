#pragma once

// WP-8.1 froxel fog on the GPU: the records shared by the C++ host code, the CPU reference
// (froxel_fog_kernel.hpp) and the compute kernels (shaders/volumetric/fog_common.{glsl,slang} declare
// the same fields in the same order; fuse_rp_volumetric_gpu_layout checks names, order and offsets).
// Vulkan-free: builds in the stub backend.
//
// Storage: every froxel quantity lives in one device-local work buffer reached through buffer device
// addresses, as f32x4 per froxel, so each pass computes in the CPU reference's f32 and the parity gates
// compare like with like:
//   current      fog.inject    (in-scattered radiance x scattering coefficient .rgb, extinction .a)
//   history[2]   fog.temporal  (the same quantities, temporally accumulated; ping-pong by frame)
//   integrated   fog.integrate (in-scattering reaching the camera .rgb, transmittance .a) at the FAR
//                              boundary of every slice, front to back along each froxel column
// Froxel index = (y * gridX + x) * gridZ + z (slice fastest, the B5 FroxelGridLayout::froxelIndex and
// the clustered grid's cluster_index order).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::volumetric_gpu {

/// Grid limits (the frame constants carry a slice-depth table of kFogMaxSlices + 1 entries).
inline constexpr u32 kFogMaxGridX = 256u;
inline constexpr u32 kFogMaxGridY = 256u;
inline constexpr u32 kFogMaxSlices = 128u;
/// Local fog volumes per frame.
inline constexpr u32 kFogMaxVolumes = 8u;
/// Workgroup sizes: fog.inject / fog.temporal / fog.integrate are linear (one froxel / one column per
/// thread), fog.apply runs 8 x 8 pixel tiles.
inline constexpr u32 kFogLinearGroup = 64u;
inline constexpr u32 kFogTile = 8u;
/// Jitter sequence length (Halton 2 / 3 / 5, indices 1..kFogJitterPeriod).
inline constexpr u32 kFogJitterPeriod = 16u;

/// FogFrameConstants::flags
enum FogFlag : u32 {
    kFogFlagHistory = 1u << 0,    ///< fog.temporal blends the previous history (else it copies the current)
    kFogFlagReproject = 1u << 1,  ///< history is reprojected through the previous camera (else same froxel)
    kFogFlagLights = 1u << 2,     ///< `lighting` is set: the directional list + the cluster lists
    kFogFlagShadows = 1u << 3,    ///< `shadows` is set: each light x its VSM visibility (WP-3.2)
    kFogFlagReversedZ = 1u << 4,  ///< RT4 is infinite-far reversed Z (else forward z / w in [0, 1])
};

/// FogVolume::shape
enum FogVolumeShape : u32 {
    kFogVolumeSphere = 0u, ///< radius = halfExtent[0]
    kFogVolumeBox = 1u,    ///< world-axis-aligned box
};

/// One local fog volume (64 bytes): extinction `density` x a linear edge ramp over the outer `edge`
/// fraction of the shape (1 inside, 0 on the surface), scattering albedo `albedo`.
struct FogVolume {
    f32 center[3] = {0.f, 0.f, 0.f};
    u32 shape = kFogVolumeSphere;
    f32 halfExtent[3] = {1.f, 1.f, 1.f};
    f32 density = 0.f;
    f32 albedo[3] = {1.f, 1.f, 1.f};
    f32 edge = 0.25f;
    f32 reserved[4] = {0.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(FogVolume) == 64u, "FogVolume layout");

/// Per-frame constants, one host-visible ring slot per frame in flight, read through BDA (1360 bytes).
struct FogFrameConstants {
    // --- addresses (BDA; 0 = absent) -----------------------------------------------------------------
    u64 lighting = 0;    ///< WP-2.1 LightingFrameConstants of this frame (cluster lists, lights, camera)
    u64 shadows = 0;     ///< WP-3.2 VsmShadowConstants (0 = unshadowed)
    u64 current = 0;     ///< f32x4[froxelCount]: fog.inject output
    u64 historyPrev = 0; ///< f32x4[froxelCount]: last frame's fog.temporal output
    u64 historyCur = 0;  ///< f32x4[froxelCount]: this frame's fog.temporal output
    u64 integrated = 0;  ///< f32x4[froxelCount]: fog.integrate output
    u64 dump = 0;        ///< f32x4[width * height]: fog.apply result before RGBA16F rounding (optional)
    u64 reserved0 = 0;
    // --- grid, extent, handles -------------------------------------------------------------------------
    u32 gridX = 0;
    u32 gridY = 0;
    u32 gridZ = 0;
    u32 froxelCount = 0;
    u32 width = 0;      ///< fog.apply extent (the depth / lit images)
    u32 height = 0;
    u32 inputDepth = 0; ///< bindless sampled RT4 (R32F device depth)
    u32 inputLit = 0;   ///< bindless sampled lit image (RGBA16F)
    u32 output = 0;     ///< bindless storage image (RGBA16F)
    u32 flags = 0;      ///< FogFlag
    u32 frameIndex = 0;
    u32 volumeCount = 0;
    // --- camera (clustered_kernel::CameraView of the fog camera) ----------------------------------------
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 nearPlane = 0.1f; ///< first slice's near depth (the volume is integrated from the camera)
    f32 right[3] = {1.f, 0.f, 0.f};
    f32 farPlane = 64.f;  ///< last slice's far depth (the fog range)
    f32 up[3] = {0.f, 1.f, 0.f};
    f32 tanX = 1.f;
    f32 back[3] = {0.f, 0.f, 1.f};
    f32 tanY = 1.f;
    // --- previous camera (reprojection) ----------------------------------------------------------------
    f32 prevPosition[3] = {0.f, 0.f, 0.f};
    f32 reserved1 = 0.f;
    f32 prevRight[3] = {1.f, 0.f, 0.f};
    f32 prevTanX = 1.f;
    f32 prevUp[3] = {0.f, 1.f, 0.f};
    f32 prevTanY = 1.f;
    f32 prevBack[3] = {0.f, 0.f, 1.f};
    f32 reserved2 = 0.f;
    // --- temporal / medium -------------------------------------------------------------------------------
    f32 jitter[3] = {0.5f, 0.5f, 0.5f}; ///< this frame's sample offset inside every froxel, [0, 1)
    f32 temporalAlpha = 0.05f;           ///< weight of the current sample in the history blend
    f32 density = 0.f;                   ///< global height fog: density x exp(-heightFalloff x max(0, y - baseHeight))
    f32 heightFalloff = 0.f;
    f32 baseHeight = 0.f;
    f32 anisotropy = 0.f;                ///< Henyey-Greenstein g, clamped to [-0.99, 0.99]
    f32 albedo[3] = {1.f, 1.f, 1.f};     ///< global fog scattering albedo
    f32 invWidth = 0.f;                  ///< 1 / width (fog.apply pixel centre -> screen fraction)
    f32 ambient[3] = {0.f, 0.f, 0.f};    ///< isotropic ambient radiance scattered by the medium
    f32 depthNear = 0.1f;                ///< RT4 linearisation (the projection's near / far)
    f32 depthFar = 1000.f;
    f32 invGridX = 0.f;                  ///< 1 / gridX, 1 / gridY (froxel -> screen fraction)
    f32 invGridY = 0.f;
    f32 invHeight = 0.f;                 ///< 1 / height
    /// [s] = near depth of slice s, [gridZ] = far depth of the last slice (the B5 FroxelSliceLayout /
    /// clustered slice_near_z distribution nearPlane x (farPlane / nearPlane)^(s / gridZ), host-computed).
    f32 sliceDepth[kFogMaxSlices + 4] = {};
    FogVolume volumes[kFogMaxVolumes] = {};
};
static_assert(sizeof(FogFrameConstants) == 1360u, "FogFrameConstants layout (fog_common mirrors it)");
static_assert(offsetof(FogFrameConstants, gridX) == 64u && offsetof(FogFrameConstants, position) == 112u &&
                  offsetof(FogFrameConstants, prevPosition) == 176u && offsetof(FogFrameConstants, jitter) == 240u &&
                  offsetof(FogFrameConstants, sliceDepth) == 320u && offsetof(FogFrameConstants, volumes) == 848u,
              "FogFrameConstants offsets");

/// Push constants (16 bytes).
struct FogPush {
    u64 frame = 0; ///< BDA of this frame's FogFrameConstants
    u64 reserved = 0;
};
static_assert(sizeof(FogPush) == 16u, "FogPush layout");

} // namespace fuse::renderer::volumetric_gpu
