#pragma once

// WP-6.0 acceleration structures: records shared by the C++ side, the kernels (shaders/rt/*) and the
// CPU reference (docs/unification/RENDERER-EXECUTION.md, renderer plan Phase 6, tier T2).
//
// Device-safe (only <fuse/types.hpp> and the GPU-scene record layouts): the instance packing below is
// the single source of truth for the GPU kernel (rt_instances.{comp,slang}) and the CPU fallback,
// and the fuse_rp_rt gates compare the two byte for byte.
//
// TLAS instance layout (one VkAccelerationStructureInstanceKHR per GPU-scene instance SLOT):
//   TLAS instance index   == GPU-scene instance slot   (rayQueryGetIntersectionInstanceIdEXT)
//   instanceCustomIndex   == slot (24 bits)            (rayQueryGetIntersectionInstanceCustomIndexEXT)
//   mask                  == RtInstanceMask bits from GpuInstanceFlag; for a DEAD slot (free, mesh
//                            without BLAS, neither visible nor casting) the inactive mask: 0, or
//                            kRtMaskDead on drivers with the inactive-instance quirk (packRtInstance)
//   flags                 == TRIANGLE_FACING_CULL_DISABLE | FORCE_OPAQUE
//   reference             == BLAS device address of the instance's mesh; for a mask-0 slot the
//                            `inactiveBlas` placeholder (any live BLAS, see packRtInstance)
//   transform             == GpuTransform rows (the scene's row-major 3x4 is Vulkan's layout)
// BLAS primitive index == mesh triangle t == MTRI entry t (gpu_scene_types.hpp "Index layout"), so
// a hit's (instance, primitive) pair names the same triangle as the visibility buffer's.

#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::rt {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;

/// Bits of the 8-bit TLAS instance mask (ray queries pass a cull mask against it).
enum RtInstanceMask : u32 {
    kRtMaskVisible = 1u << 0,     ///< kInstanceVisible
    kRtMaskShadow = 1u << 1,      ///< kInstanceCastShadow
    kRtMaskTransparent = 1u << 2, ///< kInstanceTransparent (with visible or shadow)
    /// Reserved: dead slots on drivers with the inactive-instance quirk. Never part of a cull mask.
    kRtMaskDead = 1u << 7,
    /// Every live bit (ray cull masks must not include kRtMaskDead; the probe clears it).
    kRtMaskAll = 0x7Fu,
};

/// VkGeometryInstanceFlagBitsKHR values used by every instance.
inline constexpr u32 kRtInstanceFlagCullDisable = 0x1u; ///< TRIANGLE_FACING_CULL_DISABLE
inline constexpr u32 kRtInstanceFlagForceOpaque = 0x4u; ///< FORCE_OPAQUE
inline constexpr u32 kRtInstanceFlags = kRtInstanceFlagCullDisable | kRtInstanceFlagForceOpaque;
inline constexpr u32 kRtMaxInstances = 1u << 24; ///< instanceCustomIndex is 24 bits

/// VkAccelerationStructureInstanceKHR, byte for byte (64 bytes, 16-byte aligned array stride).
struct AsInstance {
    f32 transform[3][4] = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    u32 customIndexMask = 0; ///< instanceCustomIndex (bits 0..23) | mask << 24
    u32 sbtOffsetFlags = 0;  ///< instanceShaderBindingTableRecordOffset (bits 0..23) | flags << 24
    u64 blasAddress = 0;     ///< accelerationStructureReference (0 = inactive instance)
};
static_assert(sizeof(AsInstance) == 64u, "AsInstance must match VkAccelerationStructureInstanceKHR");

/// TLAS mask of a GPU-scene instance (0 = never hit). Free slots and meshes without BLAS get 0.
FUSE_HOST_DEVICE inline u32 rtInstanceMask(u32 instanceFlags, u64 blasAddress) {
    if ((instanceFlags & gpu_scene::kInstanceValid) == 0u || blasAddress == 0u) {
        return 0u;
    }
    u32 mask = 0u;
    if ((instanceFlags & gpu_scene::kInstanceVisible) != 0u) {
        mask |= kRtMaskVisible;
    }
    if ((instanceFlags & gpu_scene::kInstanceCastShadow) != 0u) {
        mask |= kRtMaskShadow;
    }
    if (mask != 0u && (instanceFlags & gpu_scene::kInstanceTransparent) != 0u) {
        mask |= kRtMaskTransparent;
    }
    return mask;
}

/// Packs instance `slot` (the CPU fallback and the GPU kernel's definition). `blasAddresses` has
/// `blasCount` entries indexed by mesh (0 = no BLAS).
///
/// A DEAD slot (free, no BLAS, neither visible nor casting) must never be hit. Default (spec): mask 0,
/// reference 0 ("inactive instance"). Drivers with the inactive-instance quirk (RtCapabilities::
/// inactiveInstanceQuirk: Mesa lavapipe 25.2 drops the LAST N active instances of a TLAS that holds N
/// inactive ones, reference 0 or mask 0 alike; found by the fuse_rp_rt parity gate) get
/// `inactiveMask` = kRtMaskDead and `inactiveBlas` = any live BLAS instead: an active instance no ray
/// can hit, because no cull mask contains kRtMaskDead.
FUSE_HOST_DEVICE inline AsInstance packRtInstance(const GpuInstance& instance, const GpuTransform& transform, u32 slot,
                                                  const u64* blasAddresses, u32 blasCount, u64 inactiveBlas = 0u,
                                                  u32 inactiveMask = 0u) {
    AsInstance out{};
    const bool valid = (instance.flags & gpu_scene::kInstanceValid) != 0u;
    const u64 blas = valid && instance.mesh < blasCount ? blasAddresses[instance.mesh] : 0u;
    const u32 mask = rtInstanceMask(instance.flags, blas);
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            out.transform[r][c] = transform.rows[r][c];
        }
    }
    const bool dead = mask == 0u;
    const bool placeholder = dead && inactiveBlas != 0u && inactiveMask != 0u;
    out.customIndexMask = (slot & 0x00FFFFFFu) | ((dead ? (placeholder ? inactiveMask : 0u) : mask) << 24);
    out.sbtOffsetFlags = kRtInstanceFlags << 24;
    out.blasAddress = dead ? (placeholder ? inactiveBlas : 0u) : blas;
    return out;
}

/// Exact dequantisation of a WP-1.2 VPOS vertex (the BLAS vertex input; same expression as
/// vis_decode_kernel.hpp mesh_position and rt_decode.{comp,slang}).
FUSE_HOST_DEVICE inline void rtDecodePosition(const GpuMesh& mesh, const u16* vpos, u32 v, f32 out[3]) {
    out[0] = mesh.quantOffset[0] + static_cast<f32>(vpos[v * 4u + 0u]) * mesh.quantStep[0];
    out[1] = mesh.quantOffset[1] + static_cast<f32>(vpos[v * 4u + 1u]) * mesh.quantStep[1];
    out[2] = mesh.quantOffset[2] + static_cast<f32>(vpos[v * 4u + 2u]) * mesh.quantStep[2];
}

/// One probe ray (rt_probe kernel input, 32 bytes).
struct RtProbeRay {
    f32 origin[3] = {0.f, 0.f, 0.f};
    f32 tMin = 0.f;
    f32 direction[3] = {0.f, 0.f, 1.f};
    f32 tMax = 1.0e30f;
};
static_assert(sizeof(RtProbeRay) == 32u, "RtProbeRay layout (rt_probe.comp / .slang)");

enum RtProbeHitFlag : u32 {
    kRtHit = 1u << 0,       ///< committed triangle hit
    kRtFrontFace = 1u << 1, ///< rayQueryGetIntersectionFrontFaceEXT
};

/// One probe result (rt_probe kernel output, 32 bytes). Misses: flags 0, ids kInvalidIndex, t = -1.
struct RtProbeHit {
    f32 t = -1.f;
    u32 instance = gpu_scene::kInvalidIndex;    ///< TLAS instance index (== GPU-scene slot)
    u32 customIndex = gpu_scene::kInvalidIndex; ///< instanceCustomIndex (== slot)
    u32 primitive = gpu_scene::kInvalidIndex;   ///< BLAS triangle (== mesh triangle / MTRI entry)
    f32 u = 0.f;                                ///< barycentrics of vertices 1 and 2
    f32 v = 0.f;
    u32 flags = 0;                              ///< RtProbeHitFlag
    u32 geometry = gpu_scene::kInvalidIndex;    ///< geometry index inside the BLAS (always 0)
};
static_assert(sizeof(RtProbeHit) == 32u, "RtProbeHit layout (rt_probe.comp / .slang)");

// --- kernel push constants (shaders/rt/*) --------------------------------------------------------------

/// rt_decode: VPOS (u16x4) -> f32x3 BLAS vertex input.
struct RtDecodePush {
    u64 src = 0; ///< GpuMesh::positions
    u64 dst = 0; ///< f32 x 3 per vertex
    u32 vertexCount = 0;
    f32 quantOffset[3] = {0.f, 0.f, 0.f};
    f32 quantStep[3] = {1.f, 1.f, 1.f};
    u32 pad = 0;
};
static_assert(sizeof(RtDecodePush) == 48u, "RtDecodePush layout");

/// rt_instances: GPU-scene instances + transforms + BLAS address table -> AsInstance[count].
struct RtInstancesPush {
    u64 instances = 0;  ///< GpuInstance table (GpuSceneHeader::addresses[Instances])
    u64 transforms = 0; ///< GpuTransform table
    u64 blasTable = 0;  ///< u64 per mesh
    u64 out = 0;        ///< AsInstance[count]
    u32 count = 0;      ///< instance slots (GpuScene::instanceHighWater())
    u32 blasCount = 0;
    u64 inactiveBlas = 0; ///< packRtInstance `inactiveBlas`
    u32 inactiveMask = 0; ///< packRtInstance `inactiveMask`
    u32 pad[3] = {0u, 0u, 0u};
};
static_assert(sizeof(RtInstancesPush) == 64u, "RtInstancesPush layout");

/// rt_probe: one ray query per ray.
struct RtProbePush {
    u64 tlas = 0; ///< VkAccelerationStructureKHR device address
    u64 rays = 0; ///< RtProbeRay[count]
    u64 hits = 0; ///< RtProbeHit[count]
    u32 count = 0;
    u32 cullMask = kRtMaskAll;
    u32 rayFlags = 0; ///< gl_RayFlags* (opaque is implied by the instance flags)
    u32 pad[3] = {0u, 0u, 0u};
};
static_assert(sizeof(RtProbePush) == 48u, "RtProbePush layout");

inline constexpr u32 kRtWorkgroupSize = 64u;

} // namespace fuse::renderer::rt
