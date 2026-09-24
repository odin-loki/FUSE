// WP-1.5 CPU references: see include/fuse/renderer/material_resolve/resolve_reference.hpp.
#include <fuse/renderer/material_resolve/resolve_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::renderer::material_resolve {

void make_mesh_streams(const geometry::MeshletMesh& mesh, ResolveMeshData& out) {
    out.meshletTriangleOffsets.resize(mesh.meshlets.size());
    for (usize i = 0; i < mesh.meshlets.size(); ++i) {
        out.meshletTriangleOffsets[i] = mesh.meshlets[i].triangle_offset;
    }
    out.submeshes.resize(mesh.submeshes.size());
    for (usize i = 0; i < mesh.submeshes.size(); ++i) {
        const geometry::SubmeshRange& s = mesh.submeshes[i];
        out.submeshes[i] = gpu_scene::GpuSubmesh{s.meshlet_offset, s.meshlet_count, s.material_index, s.triangle_count};
    }
    resolve_kernel::MeshStreams& s = out.streams;
    s = resolve_kernel::MeshStreams{};
    s.vpos = mesh.positions.data();
    s.normals = mesh.normals.empty() ? nullptr : mesh.normals.data();
    s.tangents = mesh.tangents.empty() ? nullptr : mesh.tangents.data();
    s.uvs = mesh.uvs.empty() ? nullptr : mesh.uvs.data();
    s.vertexCount = mesh.vertex_count();
    s.submeshes = out.submeshes.empty() ? nullptr : out.submeshes.data();
    s.meshletTriangleOffsets = out.meshletTriangleOffsets.empty() ? nullptr : out.meshletTriangleOffsets.data();
}

ResolveSceneView resolve_scene_view(const gpu_scene::GpuScene& scene,
                                    const std::vector<resolve_kernel::MeshStreams>& streams) {
    using gpu_scene::GpuSceneTable;
    ResolveSceneView v{};
    const gpu_scene::TableBytes instances = scene.tableBytes(GpuSceneTable::Instances);
    const gpu_scene::TableBytes transforms = scene.tableBytes(GpuSceneTable::Transforms);
    const gpu_scene::TableBytes prev = scene.tableBytes(GpuSceneTable::PrevTransforms);
    const gpu_scene::TableBytes meshes = scene.tableBytes(GpuSceneTable::Meshes);
    const gpu_scene::TableBytes materials = scene.tableBytes(GpuSceneTable::Materials);
    v.instances = {reinterpret_cast<const gpu_scene::GpuInstance*>(instances.data), instances.count};
    v.transforms = {reinterpret_cast<const gpu_scene::GpuTransform*>(transforms.data), transforms.count};
    v.prevTransforms = {reinterpret_cast<const gpu_scene::GpuTransform*>(prev.data), prev.count};
    v.meshes = {reinterpret_cast<const gpu_scene::GpuMesh*>(meshes.data), meshes.count};
    v.materials = {reinterpret_cast<const Material::GPUMaterial*>(materials.data), materials.count};
    v.indices = {scene.indexData(), scene.indexCount()};
    v.streams = {streams.data(), static_cast<u32>(streams.size())};
    return v;
}

namespace {

resolve_kernel::Params makeParams(const ResolveSceneView& scene, const u32* vis, u32 width, u32 height) {
    resolve_kernel::Params p{};
    p.vis = {vis, width * height * 2u};
    p.instances = scene.instances;
    p.transforms = scene.transforms;
    p.prevTransforms = scene.prevTransforms;
    p.meshes = scene.meshes;
    p.indices = scene.indices;
    p.streams = scene.streams;
    p.materials = scene.materials;
    p.width = width;
    p.height = height;
    p.tilesX = (width + kTileSize - 1u) / kTileSize;
    p.tilesY = (height + kTileSize - 1u) / kTileSize;
    return p;
}

} // namespace

void attributes_reference(const ResolveSceneView& scene, const f32 viewProj[16], const f32 prevViewProj[16],
                          const u32* vis, u32 width, u32 height, std::vector<ResolveAttributeTexel>& out,
                          kernel::Backend backend) {
    const u32 pixels = width * height;
    out.assign(pixels, ResolveAttributeTexel{});
    resolve_kernel::Params p = makeParams(scene, vis, width, height);
    std::memcpy(p.viewProj, viewProj, sizeof(p.viewProj));
    std::memcpy(p.prevViewProj, prevViewProj, sizeof(p.prevViewProj));
    p.out = {out.data(), pixels};
    kernel::launch(backend, resolve_kernel::make_attributes_launch(width, height), resolve_kernel::AttributesKernel{}, p);
}

void classify_reference(const ResolveSceneView& scene, const u32* vis, u32 width, u32 height, std::vector<u32>& tileBins,
                        kernel::Backend backend) {
    resolve_kernel::Params p = makeParams(scene, vis, width, height);
    tileBins.assign(static_cast<usize>(p.tilesX) * p.tilesY, kBinEmpty);
    p.tileBins = {tileBins.data(), static_cast<u32>(tileBins.size())};
    kernel::launch(backend, resolve_kernel::make_classify_launch(p.tilesX, p.tilesY), resolve_kernel::ClassifyKernel{}, p);
}

void tile_lists(const std::vector<u32>& tileBins, u32 tilesX, std::vector<u32> (&lists)[kBinCount]) {
    for (std::vector<u32>& l : lists) {
        l.clear();
    }
    for (usize t = 0; t < tileBins.size(); ++t) {
        const u32 bin = tileBins[t];
        if (bin < kBinCount) {
            lists[bin].push_back(pack_tile(static_cast<u32>(t % tilesX), static_cast<u32>(t / tilesX)));
        }
    }
    for (std::vector<u32>& l : lists) {
        std::sort(l.begin(), l.end());
    }
}

} // namespace fuse::renderer::material_resolve
