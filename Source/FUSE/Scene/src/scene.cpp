#include <fuse/scene/scene.hpp>

namespace fuse::scene {

Scene::Scene(std::string name) : m_name(std::move(name)) {}

void Scene::setName(std::string name) {
    m_name = std::move(name);
}

void Scene::addEntity(std::string entityName, SceneEntityTransform transform) {
    SceneEntity entity;
    entity.name = std::move(entityName);
    entity.transform = transform;
    m_entities.push_back(std::move(entity));
}

void Scene::addObjectName(std::string objectName) {
    addEntity(std::move(objectName));
}

void Scene::rebuildObjectNameCache_() const {
    m_objectNamesCache.clear();
    m_objectNamesCache.reserve(m_entities.size());
    for (const SceneEntity& entity : m_entities) {
        m_objectNamesCache.push_back(entity.name);
    }
}

const std::vector<std::string>& Scene::objectNames() const {
    rebuildObjectNameCache_();
    return m_objectNamesCache;
}

void Scene::clearEntities() {
    m_entities.clear();
}

void Scene::clearObjects() {
    clearEntities();
}

} // namespace fuse::scene
