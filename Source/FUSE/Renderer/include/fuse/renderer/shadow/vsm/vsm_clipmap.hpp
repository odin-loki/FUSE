#pragma once

// WP-3.1 virtual shadow maps: host-side clipmap placement and the CPU reference of a whole VSM
// frame (no Vulkan; also built in the stub backend).
//
//   VsmClipmap        builds a frame's VsmFrameConstants geometry from the light direction and the
//                     camera: the stabilised light basis (CascadeLightSpaceLayout::buildStableLightView,
//                     shared with the CSM fallback), per-level page size (level 0 window =
//                     firstLevelExtent light-space units, x2 per level), window origins snapped to the
//                     page grid, depth keys, last frame's placement and the invalidate-all flag (light
//                     rotation changed). VirtualShadowMap uses it; tests use it without a device.
//   markReference     vsm_kernel.hpp's mark kernel over a depth image -> request bitmask.
//   VsmInvalidationReference
//                     the bounds kernel over the GPU scene's CPU mirror + last frame's records ->
//                     invalidation bitmask (the pages whose cached bit vsm.invalidate clears).
//   frameInput        VsmFrameConstants -> core_logic::VsmFrameInput for the page model
//                     (core_logic/vsm_pages: VsmPagePool::update is the allocation reference).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/core_logic/vsm_pages.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/shadow/vsm/vsm_types.hpp>

#include <vector>

namespace fuse::renderer::vsm {

/// The page model at the GPU's full capacity (16 levels, 4096 physical pages; ~1.2 MiB, allocate
/// once). A pool with fewer levels / pages is reset(levels, physPages).
using VsmPageModel = core_logic::VsmPagePool<kPagesPerAxis, kMaxLevels, kMaxPhysPages>;

struct VsmClipmapDesc {
    u32 levels = kMaxLevels;       ///< 1..16
    f32 firstLevelExtent = 8.f;    ///< light-space width of level 0's window (16k texels)
    f32 markRadiusTexels = 0.f;    ///< marking footprint half-size in texels (<= 64)
    f32 texelsPerPixel = 1.f;      ///< screen-density target: shadow texels per depth pixel (> 0)
    s32 lodBias = 0;               ///< added to the density level (never finer than containment)
};

/// What a frame's placement depends on.
struct VsmViewDesc {
    f32 lightDirection[3] = {0.f, -1.f, 0.f}; ///< direction the light travels (world)
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};  ///< clipmap centre (world)
    f32 invViewProj[16] = {};                 ///< column-major inverse of the depth buffer's viewProj
    u32 depthWidth = 0;
    u32 depthHeight = 0;
    /// World size of one depth pixel per unit of view distance (perspective: 2 tan(fovY / 2) /
    /// depthHeight). Drives the screen-density level; 0 = containment only (finest window that holds
    /// the point: 16k texels per window, far finer than the screen).
    f32 pixelSpread = 0.f;
    bool invalidateAll = false;               ///< drop every cached page this frame
};

class VsmClipmap {
public:
    /// false for an invalid description (levels, extent, radius).
    bool init(const VsmClipmapDesc& desc);
    /// Forgets the placement history (the next frame is a first frame).
    void reset();
    /// Fills the geometry fields of `c` (depthToLight, lightRotation, cameraLight, level selection,
    /// markRadiusPages, ndc scales, levels, frame, flags, level[]). Leaves handles, addresses and
    /// work offsets alone. false (nothing changed) for a degenerate light direction, an empty depth
    /// extent or a camera beyond the origin range.
    bool build(const VsmViewDesc& view, VsmFrameConstants& c);
    const VsmClipmapDesc& desc() const { return m_desc; }
    u32 frame() const { return m_frame; }

private:
    VsmClipmapDesc m_desc{};
    bool m_valid = false;
    bool m_hasPrev = false;
    u32 m_frame = 0;
    f32 m_prevRotation[9] = {};
    s32 m_prevOrigin[kMaxLevels][2] = {};
    s32 m_prevDepthKey[kMaxLevels] = {};
};

/// Mark kernel over a depth image (row-major, width x height) into `requestWords`
/// ((c.virtualPages + 31) / 32 words, cleared first). Returns the number of set bits. `backend`:
/// CpuReference or CpuParallel (bit-identical). `scratch` (4 u32 per pixel) is resized if needed.
u32 markReference(const VsmFrameConstants& c, const f32* depth, u32 width, u32 height, u32* requestWords,
                  kernel::Backend backend, std::vector<u32>& scratch);

/// Tracks last frame's bounds records like the GPU's ping-pong buffer.
class VsmInvalidationReference {
public:
    /// Forget the history (mirror of the GPU clearing its bounds buffer: every tracked instance then
    /// invalidates its footprint once).
    void reset() { m_prev.clear(); }
    /// Computes this frame's records (bounds kernel), sets the invalidation bits
    /// ((c.virtualPages + 31) / 32 words, cleared first) of every changed slot's old and new
    /// footprint, and keeps the records for the next frame. Returns the number of changed slots.
    u32 run(const VsmFrameConstants& c, const gpu_scene::GpuInstance* instances, const gpu_scene::GpuTransform* transforms,
            u32 instanceCount, const gpu_scene::GpuMesh* meshes, u32 meshCount, u32* invalidWords);
    const std::vector<VsmBoundsRecord>& records() const { return m_prev; }

private:
    std::vector<VsmBoundsRecord> m_prev;
    std::vector<VsmBoundsRecord> m_cur;
};

/// The page model's frame input for a frame's constants.
core_logic::VsmFrameInput frameInput(const VsmFrameConstants& c);

/// Sets the bit of every virtual page (in slot space) under an absolute page rectangle of a level.
void setRectBits(u32 level, const s32 rect[4], u32* words);

} // namespace fuse::renderer::vsm
