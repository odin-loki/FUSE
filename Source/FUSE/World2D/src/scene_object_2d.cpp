#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse {

SceneObject2D::SceneObject2D() : Object("SceneObject2D") {}

SceneObject2D::SceneObject2D(std::string name) : Object(std::move(name)) {}

void SceneObject2D::setPosition(float x, float y) {
    m_x = x;
    m_y = y;
}

} // namespace fuse
