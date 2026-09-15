#include <fuse/world3d/scene_object_3d.hpp>

namespace fuse {

SceneObject3D::SceneObject3D() : SceneObject2D("SceneObject3D") {}

SceneObject3D::SceneObject3D(std::string name) : SceneObject2D(std::move(name)) {}

} // namespace fuse
