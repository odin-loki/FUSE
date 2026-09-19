#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/transform_stubs.hpp>

#include <cstring>

namespace fuse {

SceneObject3D::SceneObject3D() : SceneObject2D("SceneObject3D") {}

SceneObject3D::SceneObject3D(std::string name) : SceneObject2D(std::move(name)) {}

LocalTransform3D SceneObject3D::localTransform3D() const {
    return {x(), y(), m_z, m_yawDeg};
}

WorldTransform3D SceneObject3D::worldTransform3D() const {
    const WorldTransform2D world2d = worldTransform();
    float wz = m_z;
    float wyaw = m_yawDeg;
    const Object* node = parent();
    while (node != nullptr) {
        if (std::strcmp(node->typeName(), "SceneObject3D") == 0) {
            const SceneObject3D* scene3d = static_cast<const SceneObject3D*>(node);
            wz += scene3d->z();
            wyaw += scene3d->yawDeg();
        }
        node = node->parent();
    }
    return {world2d.x, world2d.y, wz, wyaw};
}

} // namespace fuse
