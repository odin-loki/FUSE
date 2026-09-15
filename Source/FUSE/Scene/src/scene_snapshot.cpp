#include <fuse/scene/scene_snapshot.hpp>

#include <fuse/scene/scene.hpp>

namespace fuse::scene {

SceneSnapshot SceneSnapshot::capture(const Scene& scene) {
    SceneSnapshot snapshot;
    snapshot.name = scene.name();
    snapshot.camera = scene.camera();
    snapshot.entities = scene.entities();
    return snapshot;
}

void SceneSnapshot::apply(Scene& scene) const {
    scene.setName(name);
    scene.camera() = camera;
    scene.clearEntities();
    for (const SceneEntity& entity : entities) {
        scene.addEntity(entity.name, entity.transform);
    }
}

} // namespace fuse::scene
