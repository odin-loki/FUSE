#pragma once

// WP-1.5 host helpers around the CPU reference kernels (resolve_kernel.hpp): scene views built from a
// GpuScene mirror + the cooked meshlet meshes, and full-frame runs of "material_resolve_attributes"
// and "material_resolve_classify" over a read-back visibility image. fuse_rp_material_resolve compares
// the GPU attribute dump and the GPU tile lists against these.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/resolve_kernel.hpp>
#include <fuse/renderer/material_resolve/resolve_types.hpp>

#include <vector>

namespace fuse::renderer::material_resolve {

/// CPU streams of one cooked mesh. `streams` points into `mesh` and into this object: keep both
/// alive and do not move this object after make_mesh_streams().
struct ResolveMeshData {
    std::vector<u32> meshletTriangleOffsets;
    std::vector<gpu_scene::GpuSubmesh> submeshes;
    resolve_kernel::MeshStreams streams{};
};

void make_mesh_streams(const geometry::MeshletMesh& mesh, ResolveMeshData& out);

/// Scene view for the reference kernels: the GpuScene mirror (instances up to the high-water mark,
/// current and previous transforms, meshes, materials, the scene index buffer) + per-mesh streams.
struct ResolveSceneView {
    kernel::Span<const gpu_scene::GpuInstance> instances;
    kernel::Span<const gpu_scene::GpuTransform> transforms;
    kernel::Span<const gpu_scene::GpuTransform> prevTransforms;
    kernel::Span<const gpu_scene::GpuMesh> meshes;
    kernel::Span<const u32> indices;
    kernel::Span<const resolve_kernel::MeshStreams> streams;
    kernel::Span<const Material::GPUMaterial> materials;
};

ResolveSceneView resolve_scene_view(const gpu_scene::GpuScene& scene,
                                    const std::vector<resolve_kernel::MeshStreams>& streams);

/// Runs "material_resolve_attributes" over a row-major R32G32 image (2 words per pixel).
void attributes_reference(const ResolveSceneView& scene, const f32 viewProj[16], const f32 prevViewProj[16],
                          const u32* vis, u32 width, u32 height, std::vector<ResolveAttributeTexel>& out,
                          kernel::Backend backend = kernel::Backend::CpuReference);

/// Runs "material_resolve_classify": one ResolveBin per 8 x 8 tile (row-major, ceil(w/8) x ceil(h/8)).
void classify_reference(const ResolveSceneView& scene, const u32* vis, u32 width, u32 height, std::vector<u32>& tileBins,
                        kernel::Backend backend = kernel::Backend::CpuReference);

/// Sorted packed tiles (pack_tile) of each bin, from per-tile bins.
void tile_lists(const std::vector<u32>& tileBins, u32 tilesX, std::vector<u32> (&lists)[kBinCount]);

} // namespace fuse::renderer::material_resolve
