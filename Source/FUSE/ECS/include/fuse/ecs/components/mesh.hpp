#pragma once

#include <fuse/asset/asset_id.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/handle.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

struct MeshBufferTag {};
struct MeshIndexBufferTag {};

using MeshVertexBufferHandle = fuse::Handle<MeshBufferTag>;
using MeshIndexBufferHandle = fuse::Handle<MeshIndexBufferTag>;

/// Renderer-agnostic mesh component (B3.2). Buffer handles mirror `fuse::Handle` without
/// depending on `fuse_rhi`; a future bridge maps these to `renderer::BufferHandle`.
struct Mesh {
    static constexpr const char* component_name = "Mesh";

    MeshVertexBufferHandle vertex_buffer{};
    MeshIndexBufferHandle index_buffer{};
    u32 index_count = 0;
    u32 material_id = 0;

    vec3 aabb_min{};
    vec3 aabb_max{};

    bool cast_shadow = true;
    bool receive_shadow = true;
    bool visible = true;
};

/// UNI-U7-ASSET-1: the cooked assets an entity's Mesh comes from, by stable id
/// (fuse/asset/asset_id.hpp, resolved by asset::AssetRegistry). A companion component rather than
/// fields of Mesh, so the hot Mesh row keeps its size (fuse_b3_ecs_gates streams
/// Transform + Mesh + RigidBody against a bandwidth-bound floor; two u64 ids would grow Mesh by a third).
/// Absent, or invalid (0) ids = not asset-driven: Mesh's numeric vertex_buffer / material_id are
/// used as before; the asset bridge (E06) writes those from the registry for entities that have it.
struct MeshAssets {
    static constexpr const char* component_name = "MeshAssets";

    fuse::asset::AssetId mesh{};
    fuse::asset::AssetId material{};
};

} // namespace fuse::ecs
