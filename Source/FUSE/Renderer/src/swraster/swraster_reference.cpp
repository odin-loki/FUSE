// WP-5.4 CPU reference helpers: see include/fuse/renderer/swraster/swraster_reference.hpp.
#include <fuse/renderer/swraster/swraster_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_meshlets.hpp>

namespace fuse::renderer::swraster {

void SwSceneStorage::build(const gpu_scene::GpuScene& scene, const std::vector<geometry::MeshletMesh>& meshes) {
    meshlets.assign(meshes.size(), {});
    geometry.assign(meshes.size(), sw_kernel::SwMeshGeometry{});
    for (usize i = 0; i < meshes.size(); ++i) {
        const geometry::MeshletMesh& m = meshes[i];
        meshlets[i].reserve(m.meshlets.size());
        for (const geometry::MeshletRecord& r : m.meshlets) {
            meshlets[i].push_back(gpu_scene::packGpuMeshlet(r));
        }
        sw_kernel::SwMeshGeometry& g = geometry[i];
        g.meshlets = {meshlets[i].data(), static_cast<u32>(meshlets[i].size())};
        g.vertices = {m.meshlet_vertices.data(), static_cast<u32>(m.meshlet_vertices.size())};
        g.triangles = {m.meshlet_triangles.data(), static_cast<u32>(m.meshlet_triangles.size())};
        g.vpos = {m.positions.data(), static_cast<u32>(m.positions.size())};
        g.vertexCount = m.vertex_count();
    }
    const culling::SceneSpans spans = culling::scene_spans(scene);
    view.instances = spans.instances;
    view.transforms = spans.transforms;
    view.meshes = spans.meshes;
    view.geometry = {geometry.data(), static_cast<u32>(geometry.size())};
}

void swraster_classify_reference(const sw_kernel::SwSceneView& scene, const SwRasterConstants& constants,
                                 kernel::Span<const SwGroup> groups, std::vector<u32>& results, kernel::Backend backend) {
    results.assign(static_cast<usize>(groups.size) * kSwGroupSize, kSwResultNone);
    if (groups.size == 0u) {
        return;
    }
    sw_kernel::ClassifyParams p{};
    p.scene = scene;
    p.groups = groups;
    p.constants = constants;
    p.results = {results.data(), static_cast<u32>(results.size())};
    kernel::launch(backend, sw_kernel::make_classify_launch(groups.size), sw_kernel::ClassifyKernel{}, p);
}

void swraster_raster_reference(const sw_kernel::SwSceneView& scene, const SwRasterConstants& constants,
                               kernel::Span<const SwCluster> clusters, std::vector<u64>& words,
                               std::vector<SwCluster>& demoted, SwRasterReferenceStats* stats, kernel::Backend backend) {
    const usize pixels = static_cast<usize>(constants.width) * constants.height;
    if (words.size() != pixels) {
        words.assign(pixels, visbuffer::kVis64Clear);
    }
    demoted.assign(clusters.size, SwCluster{});
    u32 demotedCount = 0;
    u32 triangles = 0;
    if (clusters.size > 0u) {
        sw_kernel::RasterParams p{};
        p.scene = scene;
        p.clusters = clusters;
        p.constants = constants;
        p.target = {words.data(), static_cast<u32>(words.size())};
        p.demoted = {demoted.data(), static_cast<u32>(demoted.size())};
        p.demotedCount = &demotedCount;
        p.triangles = &triangles;
        kernel::launch(backend, sw_kernel::make_raster_launch(clusters.size), sw_kernel::RasterKernel{}, p);
    }
    demoted.resize(demotedCount < clusters.size ? demotedCount : clusters.size);
    if (stats != nullptr) {
        stats->demoted = demotedCount;
        stats->triangles = triangles;
    }
}

} // namespace fuse::renderer::swraster
