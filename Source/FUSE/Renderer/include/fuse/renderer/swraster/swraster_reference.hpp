#pragma once

// WP-5.4 host helpers around the single-source reference kernels (swraster_kernel.hpp): a CPU view of
// a GpuScene's meshlet meshes, the per-meshlet classification of read-back classify records, and the
// software rasteriser over a cluster list into a 64-bit visibility image. fuse_rp_swraster compares
// the GPU against both bit for bit.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/swraster/swraster_kernel.hpp>

#include <vector>

namespace fuse::renderer::geometry {
struct MeshletMesh;
} // namespace fuse::renderer::geometry

namespace fuse::renderer::swraster {

/// Owns the packed GpuMeshlet records and exposes an SwSceneView of a GpuScene's CPU mirror.
/// `meshes[i]` must be the meshlet mesh added as GpuScene mesh i (GpuScene::addMeshletMesh order).
struct SwSceneStorage {
    std::vector<std::vector<gpu_scene::GpuMeshlet>> meshlets;
    std::vector<sw_kernel::SwMeshGeometry> geometry;
    sw_kernel::SwSceneView view{};

    void build(const gpu_scene::GpuScene& scene, const std::vector<geometry::MeshletMesh>& meshes);
};

/// results[g * kSwGroupSize + lane] = SwResult of every meshlet of the records (resized).
void swraster_classify_reference(const sw_kernel::SwSceneView& scene, const SwRasterConstants& constants,
                                 kernel::Span<const SwGroup> groups, std::vector<u32>& results,
                                 kernel::Backend backend = kernel::Backend::CpuReference);

struct SwRasterReferenceStats {
    u32 demoted = 0;   ///< clusters the kernel would move to the HW list
    u32 triangles = 0; ///< triangles set up (not demoted)
};

/// Rasterises `clusters` with atomicMin into `words` (width * height, NOT cleared here: pass a cleared
/// or partially written image). `demoted` receives the demoted clusters (any order).
void swraster_raster_reference(const sw_kernel::SwSceneView& scene, const SwRasterConstants& constants,
                               kernel::Span<const SwCluster> clusters, std::vector<u64>& words,
                               std::vector<SwCluster>& demoted, SwRasterReferenceStats* stats = nullptr,
                               kernel::Backend backend = kernel::Backend::CpuReference);

} // namespace fuse::renderer::swraster
