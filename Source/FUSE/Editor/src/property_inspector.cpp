#include <fuse/editor/property_inspector.hpp>

#include <fuse/editor/material_property_inspect.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/object.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace fuse::editor {

namespace {

u32 componentFieldCount(const char* componentName) {
    if (componentName == nullptr) {
        return 0u;
    }

    const std::string_view name{componentName};
    if (name == ecs::Transform::component_name) {
        return 4u;
    }
    if (name == ecs::Mesh::component_name) {
        return 3u;
    }
    if (name == ecs::SDFObject::component_name) {
        return 5u;
    }
    if (name == ecs::RigidBody::component_name) {
        return 4u;
    }
    if (name == ecs::Camera::component_name) {
        return 5u;
    }
    if (name == ecs::PointLight::component_name) {
        return 4u;
    }
    if (name == ecs::DirectionalLight::component_name) {
        return 4u;
    }
    if (name == ecs::SpotLight::component_name) {
        return 6u;
    }
    return 1u;
}

} // namespace

void PropertyInspector::appendSectionIfPresent(const char* componentName, ecs::EntityID id,
                                               EditorScene& scene) {
    if (componentName == ecs::Transform::component_name && scene.registry().has<ecs::Transform>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::Mesh::component_name && scene.registry().has<ecs::Mesh>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::SDFObject::component_name && scene.registry().has<ecs::SDFObject>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::RigidBody::component_name && scene.registry().has<ecs::RigidBody>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::Camera::component_name && scene.registry().has<ecs::Camera>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::PointLight::component_name && scene.registry().has<ecs::PointLight>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::DirectionalLight::component_name &&
        scene.registry().has<ecs::DirectionalLight>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
        return;
    }
    if (componentName == ecs::SpotLight::component_name && scene.registry().has<ecs::SpotLight>(id)) {
        m_sections.push_back({componentName, componentFieldCount(componentName)});
    }
}

void PropertyInspector::sync(const EditorState& state, EditorScene& scene) {
    m_sections.clear();
    m_target = state.primarySelection;

    if (!m_target.valid() || !scene.registry().alive(m_target)) {
        m_target = ecs::EntityID::null();
        return;
    }

    appendSectionIfPresent(ecs::Transform::component_name, m_target, scene);
    appendSectionIfPresent(ecs::Mesh::component_name, m_target, scene);
    appendSectionIfPresent(ecs::SDFObject::component_name, m_target, scene);
    appendSectionIfPresent(ecs::RigidBody::component_name, m_target, scene);
    appendSectionIfPresent(ecs::Camera::component_name, m_target, scene);
    appendSectionIfPresent(ecs::PointLight::component_name, m_target, scene);
    appendSectionIfPresent(ecs::DirectionalLight::component_name, m_target, scene);
    appendSectionIfPresent(ecs::SpotLight::component_name, m_target, scene);
}

void PropertyInspector::syncRuntime(const EditorState& state, const scene::Scene& scene) {
    m_sections.clear();
    m_target = state.primarySelection;

    if (!m_target.valid() || scene.entityAt(m_target.index) == nullptr) {
        m_target = ecs::EntityID::null();
        return;
    }

    m_sections.push_back({"name", 1u});
}

bool PropertyInspector::getName(const scene::Scene& scene, std::string& out) const {
    if (!m_target.valid()) {
        return false;
    }

    const scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    out = entity->name;
    return true;
}

bool PropertyInspector::setName(std::string name, scene::Scene& scene, CommandQueue& queue) {
    if (!m_target.valid()) {
        return false;
    }

    scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    entity->name = std::move(name);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "name";
    command.propertyValue = entity->name;
    queue.post(std::move(command));
    return true;
}

bool PropertyInspector::setName(std::string name, scene::Scene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    const std::string before = entity->name;
    entity->name = std::move(name);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "name";
    command.propertyValue = entity->name;
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setTransformPosition(const ecs::vec3& position, EditorScene& scene,
                                             CommandStack& cmds) {
    if (!m_target.valid() || !scene.registry().has<ecs::Transform>(m_target)) {
        return false;
    }

    ecs::Transform* transform = scene.registry().get<ecs::Transform>(m_target);
    if (transform == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(transform->position.x) + "," +
                               formatPropertyFloat(transform->position.y) + "," +
                               formatPropertyFloat(transform->position.z);

    transform->position = position;
    transform->dirty = true;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "transform.position";
    command.propertyValue = formatPropertyFloat(position.x) + "," +
                            formatPropertyFloat(position.y) + "," + formatPropertyFloat(position.z);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::getMeshMaterialId(const EditorScene& scene, u32& out) const {
    if (!m_target.valid()) {
        return false;
    }

    const ecs::Mesh* mesh = scene.registry().get<ecs::Mesh>(m_target);
    if (mesh == nullptr) {
        return false;
    }

    out = mesh->material_id;
    return true;
}

bool PropertyInspector::tryGetMeshMaterialId(const EditorScene& scene, u32 catalogCount,
                                             u32& out) const {
    if (!getMeshMaterialId(scene, out)) {
        return false;
    }

    if (isMaterialCatalogEmpty(catalogCount) || isInvalidMaterialSlot(out, catalogCount)) {
        return false;
    }

    return true;
}

bool PropertyInspector::setMeshMaterialId(u32 materialId, EditorScene& scene, CommandStack& cmds) {
    return trySetMeshMaterialId(materialId, UINT32_MAX, scene, cmds);
}

bool PropertyInspector::trySetMeshMaterialId(u32 materialId, u32 catalogCount, EditorScene& scene,
                                             CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::Mesh* mesh = scene.registry().get<ecs::Mesh>(m_target);
    if (mesh == nullptr) {
        return false;
    }

    if (catalogCount != UINT32_MAX &&
        (isMaterialCatalogEmpty(catalogCount) || isInvalidMaterialSlot(materialId, catalogCount))) {
        return false;
    }

    const std::string before = std::to_string(mesh->material_id);
    mesh->material_id = materialId;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "mesh.material_id";
    command.propertyValue = std::to_string(materialId);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setSdfBlendAlpha(f32 alpha, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::SDFObject* sdf = scene.registry().get<ecs::SDFObject>(m_target);
    if (sdf == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(sdf->blend_alpha);
    sdf->blend_alpha = alpha;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "sdf.blend_alpha";
    command.propertyValue = formatPropertyFloat(alpha);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setDirectionalIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::DirectionalLight* light = scene.registry().get<ecs::DirectionalLight>(m_target);
    if (light == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(light->intensity);
    light->intensity = intensity;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "directional.intensity";
    command.propertyValue = formatPropertyFloat(intensity);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setSpotIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::SpotLight* light = scene.registry().get<ecs::SpotLight>(m_target);
    if (light == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(light->intensity);
    light->intensity = intensity;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "spot.intensity";
    command.propertyValue = formatPropertyFloat(intensity);
    cmds.push(std::move(command), before);
    return true;
}

} // namespace fuse::editor
