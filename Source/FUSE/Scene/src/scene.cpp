#include <fuse/scene/scene.hpp>

namespace fuse::scene {

Scene::Scene(std::string name) : m_name(std::move(name)) {}

void Scene::setName(std::string name) {
    m_name = std::move(name);
}

void Scene::addObjectName(std::string objectName) {
    m_objectNames.push_back(std::move(objectName));
}

void Scene::clearObjects() {
    m_objectNames.clear();
}

} // namespace fuse::scene
