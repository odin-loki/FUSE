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
};

} // namespace fuse::ecs
