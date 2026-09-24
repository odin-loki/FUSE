#pragma once

// WP-3.2 CPU references (no Vulkan; also built in the stub backend) of the page rasteriser and of the
// shadowed deferred shade, on the math of vsm_raster_kernel.hpp:
//
//   VsmRasterReference   caster triangles of a GpuScene CPU mirror, transformed once per frame; renders
//                        render-list pages (directional) and local pages (spot / cube faces) into CPU
//                        images laid out like the GPU's pool and local atlas.
//   shadowVisibility     the per-light lookup light.shade makes.
//   shadeShadowedReference
//                        lighting_gpu::ShadeKernel (the WP-2.1 / 2.2 deferred shade reference) with every
//                        light's contribution scaled by its shadow visibility, as light.shade does when
//                        LightingFrameDesc::shadows is set.
//
// Caster set (GPU and CPU): instances with kInstanceValid | kInstanceCastShadow, a mesh with a draw
// range in the scene index buffer and a position stream.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/shadow/vsm/vsm_types.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_kernel.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_types.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <vector>

namespace fuse::renderer::vsm {

namespace raster_math {

/// Conservative world box of a caster instance (the mesh bounding sphere grown by one quantisation
/// step per axis, through the 3x4 transform); false when the instance is not a caster (the GPU also
/// requires a position stream address, the CPU a position view).
FUSE_HOST_DEVICE inline bool caster_bounds(const gpu_scene::GpuInstance& inst, const gpu_scene::GpuMesh& mesh,
                                           const gpu_scene::GpuTransform& t, f32 c[3], f32 e[3]) {
    const u32 need = gpu_scene::kInstanceValid | gpu_scene::kInstanceCastShadow;
    if ((inst.flags & need) != need || mesh.indexCount == 0u) {
        return false;
    }
    const f32 rad = mesh.boundsRadius + ((abs_f(mesh.quantStep[0]) + abs_f(mesh.quantStep[1])) + abs_f(mesh.quantStep[2]));
    for (u32 k = 0; k < 3u; ++k) {
        const f32* row = t.rows[k];
        c[k] = ((row[0] * mesh.boundsCenter[0] + row[1] * mesh.boundsCenter[1]) + row[2] * mesh.boundsCenter[2]) + row[3];
        e[k] = rad * ((abs_f(row[0]) + abs_f(row[1])) + abs_f(row[2]));
    }
    return true;
}

/// Closed box / sphere overlap.
FUSE_HOST_DEVICE inline bool box_sphere(const f32 c[3], const f32 e[3], const f32 p[3], f32 radius) {
    const f32 dx = max_f(abs_f(c[0] - p[0]) - e[0], 0.f);
    const f32 dy = max_f(abs_f(c[1] - p[1]) - e[1], 0.f);
    const f32 dz = max_f(abs_f(c[2] - p[2]) - e[2], 0.f);
    return (dx * dx + dy * dy) + dz * dz <= radius * radius;
}

} // namespace raster_math

struct VsmRasterRefStats {
    u64 triangles = 0; ///< triangles set up (after culling)
    u64 texels = 0;    ///< covered texel samples written
    u32 pages = 0;
};

class VsmRasterReference {
public:
    /// Collects every caster triangle of `scene` in world space and, when `c` is given, in its light
    /// space (the GPU's transforms, same order of operations).
    void prepare(const visbuffer::VisSceneView& scene, const VsmFrameConstants* c);

    /// Clears each page of `list` (`count` pairs {virtual page, physical page}) to kDepthClearBits in
    /// `pool` (row-major, poolWidth texels per row) and rasterises every caster into it.
    VsmRasterRefStats renderPages(const VsmFrameConstants& c, const u32* list, u32 count, u32* pool, u32 poolWidth) const;

    /// Local pages: `count` pairs {light | face << 8, atlas page}; clears and rasterises like the GPU.
    VsmRasterRefStats renderLocal(const VsmShadowConstants& s, const u32* list, u32 count, u32* atlas, u32 atlasWidth) const;

    u32 triangleCount() const { return static_cast<u32>(m_tris.size()); }

private:
    struct CasterTri {
        f32 w[3][3];   ///< world vertices
        f32 l[3][3];   ///< light-space vertices (directional)
        f32 lmin[3];   ///< light-space bounds
        f32 lmax[3];
        u32 instance;
    };
    struct CasterInstance {
        f32 center[3];
        f32 extent[3];
        u32 first;     ///< range in m_tris
        u32 count;
    };
    std::vector<CasterTri> m_tris;
    std::vector<CasterInstance> m_instances;
};

/// Shadow inputs of the CPU shade: the frame's constants and the sampled images.
struct ShadowReferenceSource {
    const VsmShadowConstants* shadow = nullptr;
    const VsmFrameConstants* vsm = nullptr; ///< null: no directional VSM
    raster_math::ShadowStore store{};
};

/// Visibility of light `slot` at a world point (1 without a shadow).
raster_math::SampleResult shadowVisibility(const ShadowReferenceSource& src, u32 slot, const math::Vec3& position,
                                           const math::Vec3& normal);

/// The deferred shade reference with shadows (see the header comment); returns the shaded pixels.
u32 shadeShadowedReference(const lighting_gpu::ShadeReferenceDesc& desc, const ShadowReferenceSource& src,
                           std::vector<math::Vec4>& out, kernel::Backend backend = kernel::Backend::CpuParallel);

} // namespace fuse::renderer::vsm
