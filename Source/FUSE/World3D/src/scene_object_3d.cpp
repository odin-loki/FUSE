#include <fuse/world3d/scene_object_3d.hpp>

#include <cmath>

namespace fuse {

SceneObject3D::SceneObject3D() : SceneObject2D("SceneObject3D") {}

SceneObject3D::SceneObject3D(std::string name) : SceneObject2D(std::move(name)) {}

void SceneObject3D::setZ(float z) {
    math::Vec3 t = localTranslation();
    t.z = z;
    setLocalTranslation(t);
}

LocalTransform3D SceneObject3D::localTransform3D() const {
    LocalTransform3D local;
    local.x = x();
    local.y = y();
    local.z = z();
    local.yaw_deg = eulerYawDeg();
    local.pitch_deg = eulerPitchDeg();
    local.roll_deg = eulerRollDeg();
    local.rotation = localRotation();
    local.scale = localScale();
    return local;
}

WorldTransform3D SceneObject3D::worldTransform3D() const {
    WorldTransform3D world;
    world.matrix = worldMatrix();
    math::Vec3 t;
    scene_math::decomposeTRS(world.matrix, t, world.rotation, world.scale);
    world.x = t.x;
    world.y = t.y;
    world.z = t.z;
    world.yaw_deg = std::atan2(world.matrix.data[1], world.matrix.data[0]) * (180.f / 3.14159265358979323846f);
    return world;
}

const SceneObject3D* asSceneObject3D(const Object* obj) {
    if (obj == nullptr || obj->sceneNodeKind() != Object::SceneNodeKind::Node3D) {
        return nullptr;
    }
    return static_cast<const SceneObject3D*>(obj);
}

SceneObject3D* asSceneObject3D(Object* obj) {
    return const_cast<SceneObject3D*>(asSceneObject3D(static_cast<const Object*>(obj)));
}

} // namespace fuse
