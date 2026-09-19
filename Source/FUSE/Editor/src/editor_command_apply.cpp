#include <fuse/editor/editor_host.hpp>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace fuse::editor {

namespace {

constexpr f32 kEditorTickDt = 1.f / 60.f;

ecs::EntityID handleToEntity(Handle<Object> handle) {
    if (!handle.isValid()) {
        return ecs::EntityID::null();
    }

    ecs::EntityID entity{};
    entity.index = handle.index();
    entity.generation = handle.generation();
    return entity;
}

Handle<Object> entityToHandle(ecs::EntityID entity) {
    if (!entity.valid()) {
        return Handle<Object>::invalid();
    }
    return Handle<Object>(entity.index, entity.generation);
}

bool parseVec3(const std::string& text, ecs::vec3& out) {
    std::string normalized = text;
    for (char& ch : normalized) {
        if (ch == ',') {
            ch = ' ';
        }
    }

    std::istringstream stream(normalized);
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    if (!(stream >> x >> y >> z)) {
        return false;
    }

    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

void clearSelectionIfMatches(EditorState& state, ecs::EntityID entity) {
    if (state.primarySelection == entity) {
        state.primarySelection = ecs::EntityID::null();
    }

    state.selectedEntities.erase(
        std::remove(state.selectedEntities.begin(), state.selectedEntities.end(), entity),
        state.selectedEntities.end());
}

bool applySetProperty_(EditorHost& host, const EditorCommand& command) {
    if (command.propertyName == "project") {
        host.setLoadedProject(command.propertyValue);
        host.runtimeViewport().setProjectLabel(command.propertyValue);
        return true;
    }

    if (command.propertyName == "viewport.width") {
        const u32 width = static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        host.runtimeViewport().requestResize(width, 0);
        return true;
    }

    if (command.propertyName == "viewport.height") {
        const u32 height = static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        host.runtimeViewport().requestResize(0, height);
        return true;
    }

    const ecs::EntityID entity = handleToEntity(command.target);
    if (!entity.valid() || !host.editorScene().registry().alive(entity)) {
        return false;
    }

    ecs::Registry& registry = host.editorScene().registry();

    if (command.propertyName == "transform.position") {
        ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        if (transform == nullptr) {
            return false;
        }

        ecs::vec3 position{};
        if (!parseVec3(command.propertyValue, position)) {
            return false;
        }

        transform->position = position;
        transform->dirty = true;
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "mesh.material_id") {
        ecs::Mesh* mesh = registry.get<ecs::Mesh>(entity);
        if (mesh == nullptr) {
            return false;
        }

        mesh->material_id = static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "sdf.blend_alpha") {
        ecs::SDFObject* sdf = registry.get<ecs::SDFObject>(entity);
        if (sdf == nullptr) {
            return false;
        }

        sdf->blend_alpha = std::strtof(command.propertyValue.c_str(), nullptr);
        host.editorState().sceneModified = true;
        return true;
    }

    return false;
}

} // namespace

void EditorHost::setLoadedProject(std::string project) {
    m_loadedProject = std::move(project);
}

void EditorHost::applyCommand_(const EditorCommand& command) {
    ensureInitialized_();

    switch (command.kind) {
    case CommandKind::SetProperty:
        applySetProperty_(*this, command);
        break;
    case CommandKind::SelectEntity: {
        const ecs::EntityID selected = handleToEntity(command.target);
        if (selected.valid() && m_editorScene.registry().alive(selected)) {
            m_state.primarySelection = selected;
            m_state.selectedEntities.clear();
            m_state.selectedEntities.push_back(selected);
        } else if (!command.target.isValid()) {
            m_state.primarySelection = ecs::EntityID::null();
            m_state.selectedEntities.clear();
        }
        break;
    }
    case CommandKind::StartPlay:
        if (!m_playSession.isActive()) {
            m_playSession.start(m_editorScene, m_runtimeScene, m_state, m_physics);
        }
        break;
    case CommandKind::StopPlay:
        if (m_playSession.isActive()) {
            m_playSession.stop(m_editorScene, m_runtimeScene, m_state, m_physics);
        }
        break;
    case CommandKind::PausePlay:
        if (m_playSession.isPlaying()) {
            m_playSession.pause(m_runtimeScene, m_state, m_physics);
        }
        break;
    case CommandKind::ResumePlay:
        if (m_playSession.isPaused()) {
            m_playSession.resume(m_runtimeScene, m_state, m_physics);
        }
        break;
    case CommandKind::DeleteObject: {
        const ecs::EntityID entity = handleToEntity(command.target);
        if (!entity.valid() || !m_editorScene.registry().alive(entity)) {
            break;
        }

        m_undoStack.execute(std::make_unique<DeleteEntityCommand>(m_editorScene.registry(), entity));
        clearSelectionIfMatches(m_state, entity);
        m_state.sceneModified = true;
        break;
    }
    case CommandKind::ReparentObject: {
        const ecs::EntityID entity = handleToEntity(command.target);
        const ecs::EntityID newParent = handleToEntity(command.parent);
        if (!entity.valid() || !m_editorScene.registry().alive(entity)) {
            break;
        }
        if (newParent.valid() && !m_editorScene.registry().alive(newParent)) {
            break;
        }
        if (entity == newParent) {
            break;
        }

        ecs::Transform* transform = m_editorScene.registry().get<ecs::Transform>(entity);
        if (transform == nullptr) {
            break;
        }

        const ecs::EntityID oldParent = transform->parent;
        m_undoStack.execute(std::make_unique<ReparentEntityCommand>(
            m_editorScene.registry(), entity, newParent, oldParent));
        m_state.sceneModified = true;
        break;
    }
    }
}

void EditorHost::gameTick() {
    ensureInitialized_();

    m_commandsAppliedLastTick = 0;
    m_queue.drain();
    for (const EditorCommand& command : m_queue.lastDrainedBatch()) {
        applyCommand_(command);
        ++m_commandsAppliedLastTick;
    }

    m_runtimeViewport.tick(*this, kEditorTickDt);

    if (m_state.playing && !m_state.paused) {
        m_playSession.tick(kEditorTickDt, m_editorScene, m_physics);
    }

    ++m_gameTickCount;
}

} // namespace fuse::editor
