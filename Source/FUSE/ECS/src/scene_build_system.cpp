#include <fuse/ecs/systems/scene_build_system.hpp>

#include <algorithm>

namespace fuse::ecs {

SceneData SceneBuildSystem::build(Registry& reg, const CullResult& visible) {
    SceneData scene{};

    for (EntityID id : visible.visible_meshes) {
        Mesh* mesh = reg.get<Mesh>(id);
        Transform* transform = reg.get<Transform>(id);
        if (mesh == nullptr || transform == nullptr || !mesh->visible) {
            continue;
        }

        DrawItem item{};
        item.entity = id;
        item.vertex_buffer = mesh->vertex_buffer;
        item.index_buffer = mesh->index_buffer;
        item.index_count = mesh->index_count;
        item.material_id = mesh->material_id;
        item.transform = transform->local_to_world;
        scene.draw_items.push_back(item);
    }

    for (EntityID id : visible.visible_sdf_objects) {
        SDFObject* sdf = reg.get<SDFObject>(id);
        Transform* transform = reg.get<Transform>(id);
        if (sdf == nullptr || transform == nullptr || !sdf->visible) {
            continue;
        }

        SceneSdfObject item{};
        item.entity = id;
        item.type = sdf->type;
        item.params = sdf->params;
        item.material_id = sdf->material_id;
        item.transform = transform->local_to_world;
        scene.sdf_objects.push_back(item);
    }

    reg.each<DirectionalLight>([&](EntityID, DirectionalLight& light) {
        scene.sun = light;
        scene.has_sun = true;
    });

    reg.each<PointLight, Transform>([&](EntityID id, PointLight& light, Transform&) {
        if (std::find(visible.visible_lights.begin(), visible.visible_lights.end(), id) ==
            visible.visible_lights.end()) {
            return;
        }
        scene.point_lights.push_back(light);
    });

    return scene;
}

} // namespace fuse::ecs
