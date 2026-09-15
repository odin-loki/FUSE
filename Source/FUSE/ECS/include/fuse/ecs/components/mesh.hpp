#pragma once

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

} // namespace fuse::ecs
