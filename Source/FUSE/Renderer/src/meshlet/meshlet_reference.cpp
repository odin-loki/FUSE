// WP-5.1 CPU reference helpers: see include/fuse/renderer/meshlet/meshlet_reference.hpp.
#include <fuse/renderer/meshlet/meshlet_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>

namespace fuse::renderer::meshlet {

void meshlet_cull_reference(const MeshletReferenceInput& in, f32 radiusScale, f32 cutoffScale, std::vector<u32>& results,
                            kernel::Backend backend) {
    const u32 groups = in.groups.size;
    results.assign(static_cast<usize>(groups) * kMeshletTaskGroup, kMeshletResultNone);
    if (groups == 0u || in.constants == nullptr) {
        return;
    }
    cull_kernel::Params p{};
    p.instances = in.scene.instances;
    p.transforms = in.scene.transforms;
    p.prevTransforms = in.scene.prevTransforms;
    p.meshlets = in.meshlets;
    p.groups = in.groups;
    p.constants = in.constants;
    p.prevHiz = in.prevHiz;
    p.hiz = in.hiz;
    p.region = in.region;
    p.radiusScale = radiusScale;
    p.cutoffScale = cutoffScale;
    p.results = kernel::Span<u32>{results.data(), static_cast<u32>(results.size())};
    kernel::launch(backend, cull_kernel::make_launch(groups), cull_kernel::Kernel{}, p);
}

void meshlet_cull_reference_parity(const MeshletReferenceInput& in, MeshletParityReference& out, kernel::Backend backend) {
    std::vector<u32> strict;
    std::vector<u32> lenient;
    meshlet_cull_reference(in, 1.f, 1.f, out.results, backend);
    meshlet_cull_reference(in, 1.f - cull_kernel::kParityEpsilon, 1.f - cull_kernel::kParityEpsilon, strict, backend);
    meshlet_cull_reference(in, 1.f + cull_kernel::kParityEpsilon, 1.f + cull_kernel::kParityEpsilon, lenient, backend);
    out.ambiguous.assign(out.results.size(), 0u);
    out.ambiguousCount = 0;
    for (usize i = 0; i < out.results.size(); ++i) {
        if (strict[i] != lenient[i] || strict[i] != out.results[i]) {
            out.ambiguous[i] = 1u;
            ++out.ambiguousCount;
        }
    }
}

} // namespace fuse::renderer::meshlet
