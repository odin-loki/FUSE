#pragma once

#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {

struct DrawItem {
    EntityID entity = EntityID::null();
    MeshVertexBufferHandle vertex_buffer{};
    MeshIndexBufferHandle index_buffer{};
    u32 index_count = 0;
    u32 material_id = 0;
    mat4 transform = mat4::identity();
};

struct SceneSdfObject {
    EntityID entity = EntityID::null();
    SDFPrimitive type = SDFPrimitive::Sphere;
    vec3 params = {1.f, 0.f, 0.f, 0.f};
    u32 material_id = 0;
    mat4 transform = mat4::identity();
    /// CSG combine op, smooth-union blend radius, roughness and evaluation order (see
    /// `fuse/ecs/sdf_csg.hpp`); `SceneData::sdf_objects` is sorted by (csg_order, entity index).
    SDFCsgOp op = SDFCsgOp::Union;
    /// Effective smooth-union width (`sdf_effective_blend_radius`: authored radius x GRIA alpha).
    f32 blend_radius = 0.f;
    /// GRIA alpha the width was derived from (for GPU paths that re-derive or visualise it).
    f32 blend_alpha = kSdfDefaultBlendAlpha;
    f32 roughness = 0.f;
    u32 csg_order = 0;
};

struct SceneData {
    std::vector<DrawItem> draw_items;
    std::vector<SceneSdfObject> sdf_objects;
    std::vector<PointLight> point_lights;
    DirectionalLight sun{};
    bool has_sun = false;
};

class SceneBuildSystem {
public:
    static SceneData build(Registry& reg, const CullResult& visible);
    /// Same as above, writing into `out` (vectors cleared, capacity kept): heap-free per frame once
    /// the caller's SceneData has grown to the scene's visible set.
    static void build(Registry& reg, const CullResult& visible, SceneData& out);
};

} // namespace fuse::ecs
