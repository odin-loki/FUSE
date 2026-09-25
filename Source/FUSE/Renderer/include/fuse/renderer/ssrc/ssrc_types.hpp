#pragma once

// Screen-space radiance cascades (SSRC, the WP-6.5 follow-up): the records shared by the C++ host code and the
// compute kernels (shaders/ssrc/src_common.{glsl,slang} declare struct SrcFrame / the push block with the same
// fields in the same order; fuse_rp_ssrc_layout checks names, order and offsets). Vulkan-free: builds in the stub
// backend.
//
// Storage (all through buffer device addresses; one persistent work buffer, 256-aligned sections):
//   geo        f32x4 per pixel: linear view depth (0 = sky), view normal xyz (ssfx convention: +X right, +Y down,
//              +Z forward)
//   lit        f32x4 per pixel: lit radiance rgb (the WP-2.1 image), lit alpha
//   diffuse    f32x4 per pixel: diffuse albedo = albedo x (1 - metallic), material AO (RT0.w)
//   albedo     f32x4 per pixel: base albedo (RT1.rgb), 0
//   indirect   f32x4 per pixel (output): indirect diffuse radiance rgb (intensity x diffuse albedo x E / pi), AO
//   records    two ping-pong sections of cascade records, u32x2 each: f16 (r, g), f16 (b, v) with v the merged
//              visibility (the AO channel); record index (probeY * probesX + probeX) * dirRes^2 + direction
//   dirs       host-written f32x4 per direction (dx, dy, dz, solid angle) of every cascade (octahedral texel
//              centres, view space), cascade i at dirOffset[i]
// The output image (RGBA16F storage, bindless) holds either (indirect rgb, AO) or the composed lit image.

#include <fuse/types.hpp>

namespace fuse::renderer::ssrc {

/// Threads per workgroup of ssrc.cascade (1D, record per thread) / edge of the 8 x 8 pixel tiles of
/// ssrc.prepare and ssrc.gather.
inline constexpr u32 kSsrcGroupSize = 64u;
inline constexpr u32 kSsrcTile = 8u;
/// Upper bound on the cascade count (t_8 = r0 * 21845: 43690 px for r0 = 2).
inline constexpr u32 kSsrcMaxCascades = 8u;
/// Largest ssrc.cascade grid width in workgroups (the rest goes to y; 65535 is the guaranteed limit).
inline constexpr u32 kSsrcMaxGroupsX = 32768u;

/// SsrcFrameConstants::flags
enum SsrcFlag : u32 {
    kSsrcFlagBilinearFix = 1u << 0, ///< one segment per bilinear parent, ending at that parent's interval start
    kSsrcFlagReversedZ = 1u << 1,   ///< RT4 is infinite-far reversed Z (else forward z/w in [0, 1])
    kSsrcFlagDdgi = 1u << 2,        ///< ended / escaped rays add the DDGI volume's E(probe, ray dir) x ddgiScale
    kSsrcFlagCompose = 1u << 3,     ///< the output image is the composed lit image (else indirect rgb + AO)
};

/// Per-frame constants (host ring, one slot per frame in flight, read through BDA), 608 bytes.
struct SsrcFrameConstants {
    // --- work-buffer sections (BDA; 0 = absent) --------------------------------------------------------
    u64 geo = 0;
    u64 lit = 0;
    u64 diffuse = 0;
    u64 albedo = 0;
    u64 indirect = 0;
    u64 records0 = 0;
    u64 records1 = 0;
    u64 dirs = 0;       ///< direction table (f32x4 per direction, all cascades)
    u64 ddgiVolume = 0; ///< a gi_gpu::DdgiVolumeView (kSsrcFlagDdgi); 0 = none
    u64 dump = 0;       ///< optional f32x4 per pixel: the output image's value before RGBA16F rounding
    u64 reserved0 = 0;
    u64 reserved1 = 0;
    // --- extent, bindless handles, flags -------------------------------------------------------------
    u32 width = 0;
    u32 height = 0;
    u32 inputDepth = 0;      ///< RT4 (R32F device depth)
    u32 inputNormal = 0;     ///< RT0 (RGBA16F signed-octahedral world normal, material AO)
    u32 inputAlbedo = 0;     ///< RT1 (RGBA8 albedo)
    u32 inputRoughMetal = 0; ///< RT2 (RGBA8 roughness, metallic)
    u32 inputLit = 0;        ///< lit image (RGBA16F)
    u32 output = 0;          ///< storage image (RGBA16F)
    u32 flags = 0;           ///< SsrcFlag
    u32 cascades = 0;
    u32 aoCascades = 0; ///< cascades whose hits count as occlusion (AO radius = tEnd[aoCascades - 1] px)
    u32 reserved2 = 0;
    // --- camera (ssfx::SsfxCamera intrinsics + depth linearisation) ------------------------------------
    f32 fx = 1.f;
    f32 fy = 1.f;
    f32 cx = 0.f;
    f32 cy = 0.f;
    f32 nearZ = 0.01f;     ///< SsfxCamera::near_z (from the projection)
    f32 nearPlane = 0.01f; ///< device depth -> linear view depth
    f32 farPlane = 1000.f;
    f32 reserved3 = 0.f;
    f32 viewRot[12] = {};     ///< world -> engine view rotation, column c at [c * 4 + 0..2] (prepare)
    f32 viewToWorld[12] = {}; ///< engine view -> world rotation, row r at [r * 4 + 0..2] (the DDGI hook)
    f32 cameraWorld[4] = {};  ///< camera position (world)
    f32 ambient[4] = {};      ///< the lighting's ambient radiance (compose)
    f32 sky[4] = {};          ///< far-field radiance of ended / escaped rays (rgb)
    // --- march -------------------------------------------------------------------------------------------
    f32 stride = 1.f;         ///< screen-space step in pixels (major axis)
    f32 thickness = 0.5f;     ///< depth-buffer slab thickness at depth 0 (view units)
    f32 thicknessSlope = 0.f; ///< + slope x depth
    f32 maxDistance = 10.f;   ///< 3D ray length (view units)
    f32 originBias = 0.f;     ///< probe origin offset along the facing normal, x probe depth
    f32 intensity = 1.f;      ///< scale of the indirect diffuse
    f32 ddgiScale = 0.f;      ///< far-field radiance = sky + ddgiScale x E_ddgi(probe, ray direction)
    f32 planeTolerance = 0.f; ///< merge: parents farther than this x depth from the child's plane are dropped (0 = off)
    // --- cascade layout ----------------------------------------------------------------------------------
    u32 probesX[kSsrcMaxCascades] = {};
    u32 probesY[kSsrcMaxCascades] = {};
    u32 dirRes[kSsrcMaxCascades] = {};    ///< octahedral resolution (dirRes^2 directions)
    u32 dirOffset[kSsrcMaxCascades] = {}; ///< first direction of the cascade in the table
    f32 spacing[kSsrcMaxCascades] = {};   ///< probe spacing in pixels
    f32 tStart[kSsrcMaxCascades] = {};    ///< screen-space interval [tStart, tEnd) in pixels
    f32 tEnd[kSsrcMaxCascades] = {};
    f32 reserved5[kSsrcMaxCascades] = {};
};
static_assert(sizeof(SsrcFrameConstants) == 608u, "SsrcFrameConstants layout (src_common mirrors it)");

/// Push constants (40 bytes).
struct SsrcPush {
    u64 frame = 0; ///< BDA of this frame's SsrcFrameConstants
    u64 upper = 0; ///< ssrc.cascade: cascade i + 1's records (0 on the top cascade)
    u64 dst = 0;   ///< ssrc.cascade: cascade i's records
    u32 cascade = 0;
    u32 count = 0;   ///< ssrc.cascade: records of this dispatch
    u32 groupsX = 1; ///< ssrc.cascade: workgroups along x (record = (group.y * groupsX + group.x) * 64 + lane)
    u32 reserved = 0;
};
static_assert(sizeof(SsrcPush) == 40u, "SsrcPush layout");

} // namespace fuse::renderer::ssrc
