#pragma once

#include <fuse/math/mat.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Entity identity copied from `fuse::ecs::EntityID` (index + generation) without an ECS include.
struct DeferredGBufferDrawEntity {
    u32 index = 0;
    u32 generation = 0;
};

/// CPU G-buffer draw. Matches the `fuse::ecs::DrawItem` fields a caller can fill without editing
/// ECS: column-major transform (same 16-float layout as `fuse::ecs::mat4`), index count, material
/// id, and entity. Mesh vertex/index buffer handles stay on the ECS draw item.
struct DeferredGBufferDraw {
    math::Mat4 transform = math::Mat4::identity();
    u32 indexCount = 0;
    u32 materialId = 0;
    DeferredGBufferDrawEntity entity{};
};

static_assert(sizeof(DeferredGBufferDrawEntity) == sizeof(u32) * 2u,
              "entity matches fuse::ecs::EntityID index + generation");
static_assert(sizeof(math::Mat4) == sizeof(f32) * 16u,
              "transform is a column-major 4x4, memcpy-compatible with fuse::ecs::mat4");

} // namespace fuse::renderer
