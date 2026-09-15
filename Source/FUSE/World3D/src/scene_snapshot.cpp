#include <fuse/world3d/scene_snapshot.hpp>

namespace fuse::world3d {

void SceneSnapshot3D::clear() {
    m_objects.clear();
    m_visibleCount = 0;
}

void SceneSnapshot3D::reserve(u32 objectCount) {
    m_objects.reserve(objectCount);
}

void SceneSnapshot3D::addObject(const ObjectDrawCmd3D& cmd) {
    m_objects.push_back(cmd);
}

} // namespace fuse::world3d
