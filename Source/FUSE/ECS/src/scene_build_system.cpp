#include <fuse/ecs/systems/scene_build_system.hpp>

#include <fuse/ecs/sdf_csg.hpp>

#include <algorithm>

namespace fuse::ecs {

SceneData SceneBuildSystem::build(Registry& reg, const CullResult& visible) {
    SceneData scene{};
    build(reg, visible, scene);
    return scene;
}

void SceneBuildSystem::build(Registry& reg, const CullResult& visible, SceneData& scene) {
    scene.draw_items.clear();
    scene.sdf_objects.clear();
    scene.point_lights.clear();
    scene.sun = DirectionalLight{};
    scene.has_sun = false;

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
        item.op = sdf->op;
        item.blend_radius = sdf_effective_blend_radius(*sdf);
        item.blend_alpha = sdf->blend_alpha;
        item.roughness = sdf->roughness;
        item.csg_order = sdf->csg_order;
        scene.sdf_objects.push_back(item);
    }
    // CSG is order dependent (subtract carves what precedes it): hand consumers the fold order.
    // (csg_order, entity index) is a strict total order (each entity appears once), so an unstable
    // sort gives the same result without std::stable_sort's temporary heap buffer.
    std::sort(scene.sdf_objects.begin(), scene.sdf_objects.end(),
                     [](const SceneSdfObject& a, const SceneSdfObject& b) {
                         if (a.csg_order != b.csg_order) {
                             return a.csg_order < b.csg_order;
                         }
                         return a.entity.index < b.entity.index;
                     });

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
}

} // namespace fuse::ecs
