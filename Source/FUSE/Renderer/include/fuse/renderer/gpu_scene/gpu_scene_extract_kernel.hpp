#pragma once

// WP-1.1 GPU scene: ECS -> GPU scene packing as a single-source kernel (docs/compute-kernels.md),
// one entity per item over one ECS archetype chunk.
//
// Each item packs its entity's Mesh + Transform into the GpuInstance / GpuTransform records of the
// instance slot the extractor assigned it, compares them with the CPU mirror byte for byte, and
// marks the rows dirty only when they changed (TableView::write). Rows are owned by exactly one
// item (the entity -> slot map is injective) and dirty-list slots come from global_atomic_add, so
// CpuReference and CpuParallel produce identical mirrors and, after DirtySet sorts the list,
// identical upload ranges. This is the diff that makes upload bytes proportional to what moved:
// the ECS TransformSystem clears Transform::dirty before rendering, so the extractor cannot rely
// on it and compares packed records instead.
//
// Read-only on the ECS: params hold const spans of the registry's columns.
// CPU backends only (the ECS component headers are host code; no CUDA entry is provided).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>

namespace fuse::renderer::gpu_scene::extract_kernel {

inline constexpr const char* kName = "gpu_scene_extract";
inline constexpr u32 kWorkgroup = 64u;

struct Params {
    kernel::Span<const ecs::EntityID> ids;
    kernel::Span<const ecs::Transform> transforms;
    kernel::Span<const ecs::Mesh> meshes;
    kernel::Span<const u32> entitySlot; ///< entity index -> instance slot (kInvalidIndex = none)
    kernel::Span<const u32> meshRemap;  ///< Mesh::vertex_buffer index -> mesh row; empty = identity
    TableView<GpuInstance> instances;
    TableView<GpuTransform> transformsOut;
    u32 extraFlags = 0;                 ///< e.g. kInstanceStatic for TagStatic archetypes
    u32* counters = nullptr;            ///< [0] instance rows rewritten, [1] transform rows rewritten
};

/// Mesh row for an ECS Mesh component.
inline u32 resolve_mesh(const ecs::Mesh& mesh, const kernel::Span<const u32>& remap) {
    if (!mesh.vertex_buffer.isValid()) {
        return kInvalidIndex;
    }
    const u32 index = mesh.vertex_buffer.index();
    if (remap.empty()) {
        return index;
    }
    return index < remap.size ? remap[index] : kInvalidIndex;
}

inline u32 instance_flags(const ecs::Mesh& mesh, u32 extraFlags) {
    u32 flags = kInstanceValid | extraFlags;
    flags |= mesh.visible ? static_cast<u32>(kInstanceVisible) : 0u;
    flags |= mesh.cast_shadow ? static_cast<u32>(kInstanceCastShadow) : 0u;
    flags |= mesh.receive_shadow ? static_cast<u32>(kInstanceReceiveShadow) : 0u;
    return flags;
}

/// Packs one entity; `current` supplies the fields the ECS does not own (slot generation, userData).
inline GpuInstance pack_instance(ecs::EntityID id, const ecs::Mesh& mesh, const GpuInstance& current,
                                 const kernel::Span<const u32>& remap, u32 extraFlags) {
    GpuInstance out{};
    out.mesh = resolve_mesh(mesh, remap);
    out.material = mesh.material_id;
    out.flags = instance_flags(mesh, extraFlags);
    out.generation = current.generation;
    out.entityIndex = id.index;
    out.entityGeneration = id.generation;
    out.userData = current.userData;
    return out;
}

inline GpuTransform pack_transform(const ecs::Transform& transform) {
    GpuTransform out{};
    transformFromColumnMajor(transform.local_to_world.data.data(), out);
    return out;
}

struct Kernel {
    void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const ecs::EntityID id = p.ids[idx.linear];
        const u32 slot = id.index < p.entitySlot.size ? p.entitySlot[id.index] : kInvalidIndex;
        if (slot == kInvalidIndex || slot >= p.instances.count) {
            return;
        }
        const GpuInstance record = pack_instance(id, p.meshes[idx.linear], p.instances.rows[slot], p.meshRemap, p.extraFlags);
        if (p.instances.write(slot, record)) {
            kernel::global_atomic_add(&p.counters[0], 1u);
        }
        if (p.transformsOut.write(slot, pack_transform(p.transforms[idx.linear]))) {
            kernel::global_atomic_add(&p.counters[1], 1u);
        }
    }
};

inline kernel::KernelLaunch make_launch(u32 entities) {
    return kernel::KernelLaunch{kName, kernel::extent1(entities), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::gpu_scene::extract_kernel
