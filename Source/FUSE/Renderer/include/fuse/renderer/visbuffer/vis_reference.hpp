#pragma once

// WP-1.4 host helpers around the CPU reference: the decode kernel over a read-back visibility image,
// and a brute-force CPU rasteriser that says, per pixel centre, which (instance, triangle) is nearest
// and whether that answer is robust (far enough from every edge and from every other surface that a
// conforming GPU rasteriser must agree). fuse_rp_visbuffer compares the GPU targets against both.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>
#include <fuse/renderer/visbuffer/vis_types.hpp>

#include <vector>

namespace fuse::renderer::visbuffer {

/// CPU view of a scene for the decode kernel and the raster reference.
struct VisSceneView {
    kernel::Span<const gpu_scene::GpuInstance> instances;
    kernel::Span<const gpu_scene::GpuTransform> transforms;
    kernel::Span<const gpu_scene::GpuMesh> meshes;
    kernel::Span<const u32> indices;
    kernel::Span<const decode_kernel::MeshPositions> positions; ///< per mesh (VPOS, u16 x 4 per vertex)
};

/// View of a GpuScene's CPU mirror (instances up to the high-water mark, the scene index buffer)
/// with `positions[m]` = the VPOS stream of mesh m (e.g. geometry::MeshletMesh::positions).
VisSceneView vis_scene_view(const gpu_scene::GpuScene& scene, const std::vector<decode_kernel::MeshPositions>& positions);

/// Runs "visbuffer_decode" over a row-major R32G32 image (2 words per pixel).
void decode_reference(const VisSceneView& scene, const f32 viewProj[16], const u32* vis, u32 width, u32 height,
                      std::vector<VisDecodeTexel>& out, kernel::Backend backend = kernel::Backend::CpuReference);

struct RasterRefOptions {
    f64 edgeMargin = 0.05;   ///< pixels: a robust winner's centre is at least this far inside every edge
    f64 depthMargin = 1e-5;  ///< NDC depth: every other surface near the centre is at least this much farther
};

struct RasterRefPixel {
    u32 instance = kVisInvalid; ///< nearest triangle covering the pixel centre (kVisInvalid: none)
    u32 triangle = kVisInvalid;
    f32 depth = 1.f;            ///< its depth at the centre (z / w)
    u8 robust = 0;              ///< 1: any conforming rasteriser must produce this sample
};

struct RasterRefStats {
    u64 triangles = 0;
    u64 pixelTests = 0;
    u32 nearPlaneTriangles = 0; ///< triangles with a vertex at w <= 1e-6 (pixels they touch are not robust)
    u32 covered = 0;
    u32 robust = 0;
    u32 robustCovered = 0;
};

/// Rasterises every eligible instance (valid + visible, a mesh with a draw range in the scene index
/// buffer) at every pixel centre in f64: nearest depth in [0, 1] wins (forward depth, LESS). A pixel
/// is robust when its winner covers the centre with `edgeMargin`, no other triangle within
/// `edgeMargin` of the centre is nearer than winner + `depthMargin`, and the depth is at least
/// `depthMargin` inside [0, 1]; an empty pixel is robust when no triangle comes within `edgeMargin`.
void raster_reference(const VisSceneView& scene, const f32 viewProj[16], u32 width, u32 height,
                      const RasterRefOptions& options, std::vector<RasterRefPixel>& out, RasterRefStats* stats = nullptr);

} // namespace fuse::renderer::visbuffer
