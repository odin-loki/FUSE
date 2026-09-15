#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/transform_stubs.hpp>

#include <cstring>

namespace fuse {

const SceneObject2D* asSceneObject2D(const Object* obj) {
    if (obj == nullptr) {
        return nullptr;
    }
    const char* type = obj->typeName();
    if (std::strcmp(type, "SceneObject2D") == 0 || std::strcmp(type, "SceneObject3D") == 0) {
        return static_cast<const SceneObject2D*>(obj);
    }
    return nullptr;
}

LocalTransform2D SceneObject2D::localTransform() const {
    return {m_x, m_y};
}

WorldTransform2D SceneObject2D::worldTransform() const {
    float wx = m_x;
    float wy = m_y;
    const Object* node = parent();
    while (node != nullptr) {
        if (const SceneObject2D* parent2d = asSceneObject2D(node)) {
            wx += parent2d->x();
            wy += parent2d->y();
        }
        node = node->parent();
    }
    return {wx, wy};
}

} // namespace fuse
