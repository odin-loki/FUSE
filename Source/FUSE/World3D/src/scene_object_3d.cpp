#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/transform_stubs.hpp>

#include <cstring>

namespace fuse {

SceneObject3D::SceneObject3D() : SceneObject2D("SceneObject3D") {}

SceneObject3D::SceneObject3D(std::string name) : SceneObject2D(std::move(name)) {}

LocalTransform3D SceneObject3D::localTransform3D() const {
    return {x(), y(), m_z};
}

WorldTransform3D SceneObject3D::worldTransform3D() const {
    const WorldTransform2D world2d = worldTransform();
    float wz = m_z;
    const Object* node = parent();
    while (node != nullptr) {
        if (std::strcmp(node->typeName(), "SceneObject3D") == 0) {
            wz += static_cast<const SceneObject3D*>(node)->z();
        }
        node = node->parent();
    }
    return {world2d.x, world2d.y, wz};
}

} // namespace fuse
