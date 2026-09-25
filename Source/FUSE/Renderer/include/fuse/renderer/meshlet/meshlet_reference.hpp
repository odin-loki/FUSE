#pragma once

// WP-5.1 host helpers around the single-source per-meshlet reference kernel (meshlet_cull_kernel.hpp):
// the final result of every meshlet of a region's task-group records, and the parity variant that
// marks meshlets on a test boundary. fuse_rp_meshlet_path compares the GPU results buffer with it.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/meshlet/meshlet_cull_kernel.hpp>

#include <vector>

namespace fuse::renderer::meshlet {

struct MeshletReferenceInput {
    culling::SceneSpans scene{};
    /// Per mesh index, that mesh's meshlet records (the WP-1.2 MeshletMesh::meshlets).
    kernel::Span<const kernel::Span<const geometry::MeshletRecord>> meshlets;
    kernel::Span<const MeshletGroup> groups; ///< the region's records (GPU read-back)
    const MeshletConstants* constants = nullptr;
    culling::cull_kernel::HizLevels prevHiz{}; ///< last frame's pyramid (early pass)
    culling::cull_kernel::HizLevels hiz{};     ///< this frame's after the phase-1 draws
    u32 region = 0;                            ///< 0 = culler phase-1 instances, 1 = phase 2
};

/// results[g * kMeshletTaskGroup + lane] for every record (resized).
void meshlet_cull_reference(const MeshletReferenceInput& in, f32 radiusScale, f32 cutoffScale, std::vector<u32>& results,
                            kernel::Backend backend = kernel::Backend::CpuReference);

/// meshlet_cull_reference at scale 1, strict (1 - e) and lenient (1 + e); `ambiguous[i]` = 1 when the
/// strict and lenient results differ (parity rule in meshlet_cull_kernel.hpp).
struct MeshletParityReference {
    std::vector<u32> results;
    std::vector<u8> ambiguous;
    u32 ambiguousCount = 0;
};
void meshlet_cull_reference_parity(const MeshletReferenceInput& in, MeshletParityReference& out,
                                   kernel::Backend backend = kernel::Backend::CpuReference);

} // namespace fuse::renderer::meshlet
