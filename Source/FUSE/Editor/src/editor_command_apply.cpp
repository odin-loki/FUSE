#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/viewport_vulkan_surface.hpp>

#include <fuse/ai/agent_entity_bind.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/ecs/components/light.hpp>
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

std::string capturePropertyValueBefore(const EditorHost& host, const EditorCommand& command) {
    const ecs::EntityID entity = handleToEntity(command.target);
    if (!entity.valid() || !host.editorScene().registry().alive(entity)) {
        return {};
    }

    const ecs::Registry& registry = host.editorScene().registry();

    if (command.propertyName == "transform.position") {
        if (!registry.has<ecs::Transform>(entity)) {
            return {};
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        return std::to_string(transform->position.x) + "," + std::to_string(transform->position.y) +
               "," + std::to_string(transform->position.z);
    }

    if (command.propertyName == "transform.scale") {
        if (!registry.has<ecs::Transform>(entity)) {
            return {};
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        return std::to_string(transform->scale.x) + "," + std::to_string(transform->scale.y) + "," +
               std::to_string(transform->scale.z);
    }

    if (command.propertyName == "transform.rotation") {
        if (!registry.has<ecs::Transform>(entity)) {
            return {};
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        return std::to_string(transform->rotation.x) + "," + std::to_string(transform->rotation.y) +
               "," + std::to_string(transform->rotation.z) + "," +
               std::to_string(transform->rotation.w);
    }

    if (command.propertyName == "mesh.material_id") {
        if (!registry.has<ecs::Mesh>(entity)) {
            return {};
        }
        return std::to_string(registry.get<ecs::Mesh>(entity)->material_id);
    }

    if (command.propertyName == "sdf.blend_alpha") {
        if (!registry.has<ecs::SDFObject>(entity)) {
            return {};
        }
        return std::to_string(registry.get<ecs::SDFObject>(entity)->blend_alpha);
    }

    if (command.propertyName == "directional.intensity") {
        if (!registry.has<ecs::DirectionalLight>(entity)) {
            return {};
        }
        return std::to_string(registry.get<ecs::DirectionalLight>(entity)->intensity);
    }

    if (command.propertyName == "spot.intensity") {
        if (!registry.has<ecs::SpotLight>(entity)) {
            return {};
        }
        return std::to_string(registry.get<ecs::SpotLight>(entity)->intensity);
    }

    return {};
}

bool isUndoableEntityProperty(const EditorCommand& command) {
    if (command.kind != CommandKind::SetProperty || !command.target.isValid()) {
        return false;
    }

    return command.propertyName == "transform.position" || command.propertyName == "transform.scale" ||
           command.propertyName == "transform.rotation" || command.propertyName == "mesh.material_id" ||
           command.propertyName == "sdf.blend_alpha" || command.propertyName == "directional.intensity" ||
           command.propertyName == "spot.intensity";
}

bool applySetProperty_(EditorHost& host, const EditorCommand& command) {
    if (command.propertyName == "project") {
        host.setLoadedProject(command.propertyValue);
        host.runtimeViewport().setProjectLabel(command.propertyValue);
        return true;
    }

    if (command.propertyName == "project.root") {
        host.runtimeViewport().setProjectRoot(command.propertyValue);
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

    if (command.propertyName == "viewport.vk_surface_qt_stub") {
        host.runtimeViewport().setPendingQtStubSurface(command.propertyValue != "0");
        return true;
    }

    if (command.propertyName == "viewport.vk_surface_handle") {
        const u64 handleValue = std::strtoull(command.propertyValue.c_str(), nullptr, 10);
        const u32 width = host.runtimeViewport().panel().width();
        const u32 height = host.runtimeViewport().panel().height();
        ViewportVulkanSurfaceResult surfaceResult =
            createViewportVulkanSurfaceFromWinId(handleValue, width, height);
        const bool useStubPath = host.runtimeViewport().pendingQtStubSurface() || surfaceResult.stubPath;
        host.runtimeViewport().setExternalSurfaceHandle(
            surfaceResult.vkSurface, width, height,
            useStubPath ? "qt_winid_stub" : "qt_vulkan_instance",
            useStubPath, !useStubPath && !surfaceResult.stubPath, surfaceResult.vkInstance);
        host.runtimeViewport().setPendingQtStubSurface(useStubPath);
        return true;
    }

    if (command.propertyName == "ai.tree_profile_id") {
        host.setSelectedAiTreeProfileId(
            static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10)));
        return true;
    }

    if (command.propertyName == "ai.selected_agent") {
        host.setSelectedAiAgentIndex(
            static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10)));
        return true;
    }

    if (command.propertyName == "ai.agent_entity") {
        std::istringstream stream(command.propertyValue);
        std::string agentToken;
        std::string indexToken;
        std::string generationToken;
        if (!std::getline(stream, agentToken, ':') || !std::getline(stream, indexToken, ':') ||
            !std::getline(stream, generationToken)) {
            return false;
        }

        const u32 agentIndex = static_cast<u32>(std::strtoul(agentToken.c_str(), nullptr, 10));
        const u32 entityIndex = static_cast<u32>(std::strtoul(indexToken.c_str(), nullptr, 10));
        const u32 entityGeneration = static_cast<u32>(std::strtoul(generationToken.c_str(), nullptr, 10));
        host.setAiAgentEntityBinding(agentIndex, Handle<Object>(entityIndex, entityGeneration));
        return true;
    }

    if (command.propertyName == "ai.codegen_reload") {
        const std::size_t headerEnd = command.propertyValue.find('\n');
        if (headerEnd == std::string::npos) {
            return false;
        }

        const std::string header = command.propertyValue.substr(0, headerEnd);
        const std::string csText = command.propertyValue.substr(headerEnd + 1);

        u32 profileId = 0;
        std::string uaiskModule;
        std::istringstream headerStream(header);
        std::string token;
        while (std::getline(headerStream, token, ';')) {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (key == "profile") {
                profileId = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
            } else if (key == "module") {
                uaiskModule = value;
            }
        }

        if (uaiskModule.empty() || csText.empty()) {
            return false;
        }

        return host.reloadAiCodegenProfile(profileId, uaiskModule, csText);
    }

    if (command.propertyName == "ai.tree_file_reload") {
        u32 profileId = 0;
        std::string watchPath;
        std::istringstream headerStream(command.propertyValue);
        std::string token;
        while (std::getline(headerStream, token, ';')) {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (key == "profile") {
                profileId = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
            } else if (key == "path") {
                watchPath = value;
            }
        }

        if (watchPath.empty()) {
            return false;
        }

        return host.reloadAiTreeFromDisk(profileId, watchPath);
    }

    if (command.propertyName == "cinematics.seq_asset") {
        host.setLoadedCinematicsSeqAsset(command.propertyValue);
        return true;
    }

    if (command.propertyName == "cinematics.seq_scrub_preview_ms") {
        const fuse::cinematics::TimelineMs timeMs =
            static_cast<fuse::cinematics::TimelineMs>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        fuse::cinematics::Timeline timeline;
        fuse::cinematics::SeqScrubPreview preview;
        std::string error;
        if (fuse::cinematics::scrub_seq_preview(host.loadedCinematicsSeqAsset(), timeMs, timeline,
                                                preview, &error)) {
            host.setCinematicsSeqScrubPreview(timeMs, preview);
            return true;
        }
        return false;
    }

    if (command.propertyName == "cinematics.seq_preview_pane_wire") {
        const fuse::cinematics::TimelineMs timeMs =
            static_cast<fuse::cinematics::TimelineMs>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        fuse::cinematics::Timeline timeline;
        fuse::cinematics::SeqScrubPreview preview;
        std::string error;
        if (fuse::cinematics::scrub_seq_preview(host.loadedCinematicsSeqAsset(), timeMs, timeline,
                                                preview, &error)) {
            host.setCinematicsSeqScrubPreview(timeMs, preview);
            host.incrementCinematicsSeqPreviewPaneWireCount();
            return true;
        }
        return false;
    }

    const ecs::EntityID entity = handleToEntity(command.target);
    if (!entity.valid() || !host.editorScene().registry().alive(entity)) {
        return false;
    }

    ecs::Registry& registry = host.editorScene().registry();

    if (command.propertyName == "transform.position") {
        if (!registry.has<ecs::Transform>(entity)) {
            return false;
        }
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

    if (command.propertyName == "transform.scale") {
        if (!registry.has<ecs::Transform>(entity)) {
            return false;
        }
        ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        if (transform == nullptr) {
            return false;
        }

        ecs::vec3 scale{};
        if (!parseVec3(command.propertyValue, scale)) {
            return false;
        }

        transform->scale = scale;
        transform->dirty = true;
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "transform.rotation") {
        if (!registry.has<ecs::Transform>(entity)) {
            return false;
        }
        ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        if (transform == nullptr) {
            return false;
        }

        std::string normalized = command.propertyValue;
        for (char& ch : normalized) {
            if (ch == ',') {
                ch = ' ';
            }
        }

        std::istringstream stream(normalized);
        ecs::quat rotation{};
        if (!(stream >> rotation.x >> rotation.y >> rotation.z >> rotation.w)) {
            return false;
        }

        transform->rotation = rotation;
        transform->dirty = true;
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "mesh.material_id") {
        if (!registry.has<ecs::Mesh>(entity)) {
            return false;
        }
        ecs::Mesh* mesh = registry.get<ecs::Mesh>(entity);
        if (mesh == nullptr) {
            return false;
        }

        mesh->material_id = static_cast<u32>(std::strtoul(command.propertyValue.c_str(), nullptr, 10));
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "sdf.blend_alpha") {
        if (!registry.has<ecs::SDFObject>(entity)) {
            return false;
        }
        ecs::SDFObject* sdf = registry.get<ecs::SDFObject>(entity);
        if (sdf == nullptr) {
            return false;
        }

        sdf->blend_alpha = std::strtof(command.propertyValue.c_str(), nullptr);
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "directional.intensity") {
        if (!registry.has<ecs::DirectionalLight>(entity)) {
            return false;
        }
        ecs::DirectionalLight* light = registry.get<ecs::DirectionalLight>(entity);
        if (light == nullptr) {
            return false;
        }

        light->intensity = std::strtof(command.propertyValue.c_str(), nullptr);
        host.editorState().sceneModified = true;
        return true;
    }

    if (command.propertyName == "spot.intensity") {
        if (!registry.has<ecs::SpotLight>(entity)) {
            return false;
        }
        ecs::SpotLight* light = registry.get<ecs::SpotLight>(entity);
        if (light == nullptr) {
            return false;
        }

        light->intensity = std::strtof(command.propertyValue.c_str(), nullptr);
        host.editorState().sceneModified = true;
        return true;
    }

    return false;
}

} // namespace

void EditorHost::setLoadedProject(std::string project) {
    m_loadedProject = std::move(project);
}

void EditorHost::setSelectedAiTreeProfileId(u32 profileId) {
    m_selectedAiTreeProfileId = profileId;
}

void EditorHost::setSelectedAiAgentIndex(u32 agentIndex) {
    m_selectedAiAgentIndex = agentIndex;
}

bool EditorHost::reloadAiCodegenProfile(u32 profileId, const std::string& uaiskModule, const std::string& csText) {
    std::string error;
    if (!fuse::ai::uaisk::reloadCodegenProfile(uaiskModule, csText, profileId, m_pieBehaviorRuntime,
                                               fuse::ai::TreeReloadPolicy::PreserveBlackboard, &error)) {
        return false;
    }
    ++m_aiCodegenReloadCount;
    syncPieAiBindings_();
    return true;
}

bool EditorHost::reloadAiTreeFromDisk(u32 profileId, const std::string& watchPath) {
    m_aiTreeFileWatch.watchProfileFromDisk(watchPath, profileId);
    std::string error;
    u32 reloaded = m_aiTreeFileWatch.pollInotifyFileChanges(m_pieBehaviorRuntime, &error);
    if (reloaded == 0u) {
        reloaded = m_aiTreeFileWatch.pollOsFileChanges(m_pieBehaviorRuntime, &error);
    }
    if (reloaded == 0u) {
        reloaded = m_aiTreeFileWatch.pollReloads(m_pieBehaviorRuntime, &error);
    }
    if (reloaded == 0u && m_aiTreeFileWatch.entryFor(watchPath) == nullptr) {
        return false;
    }
    ++m_aiTreeFileReloadCount;
    syncPieAiBindings_();
    return true;
}

void EditorHost::setAiAgentEntityBinding(u32 agentIndex, Handle<Object> entity) {
    for (AiAgentEntityBinding& binding : m_aiAgentEntityBindings) {
        if (binding.agentIndex == agentIndex) {
            binding.entity = entity;
            syncPieAiBindings_();
            return;
        }
    }
    m_aiAgentEntityBindings.push_back({agentIndex, entity});
    syncPieAiBindings_();
}

void EditorHost::syncPieAiBindings_() {
    std::vector<fuse::ai::AgentEntityBinding> bindings;
    bindings.reserve(m_aiAgentEntityBindings.size());
    for (const AiAgentEntityBinding& binding : m_aiAgentEntityBindings) {
        bindings.push_back({binding.agentIndex, binding.entity});
    }
    fuse::ai::wireAgentEntityBindings(m_pieBehaviorRuntime, bindings);

    m_pieBehaviorRuntime.setAgentPositionProvider(
        [this](Handle<Object> entity, float& outX, float& outY) {
            ecs::EntityID ecsEntity{};
            ecsEntity.index = entity.index();
            ecsEntity.generation = entity.generation();
            if (!ecsEntity.valid() || !editorScene().registry().alive(ecsEntity)) {
                return false;
            }
            if (!editorScene().registry().has<ecs::Transform>(ecsEntity)) {
                return false;
            }
            const ecs::Transform* transform = editorScene().registry().get<ecs::Transform>(ecsEntity);
            outX = transform->position.x;
            outY = transform->position.y;
            return true;
        });

    m_pieBehaviorRuntime.syncAgentBindingsFromEntities();
}

void EditorHost::setLoadedCinematicsSeqAsset(std::string assetText) {
    m_loadedCinematicsSeqAsset = std::move(assetText);
}

void EditorHost::setCinematicsSeqScrubPreview(fuse::cinematics::TimelineMs timeMs,
                                              const fuse::cinematics::SeqScrubPreview& preview) {
    m_cinematicsSeqScrubPreviewMs = timeMs;
    m_cinematicsSeqScrubPreview = preview;
}

void EditorHost::incrementCinematicsSeqPreviewPaneWireCount() {
    ++m_cinematicsSeqPreviewPaneWireCount;
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
    case CommandKind::Undo:
        if (m_undoStack.canUndo()) {
            m_undoStack.undo();
            m_state.sceneModified = true;
        }
        break;
    case CommandKind::Redo:
        if (m_undoStack.canRedo()) {
            m_undoStack.redo();
            m_state.sceneModified = true;
        }
        break;
    }
}

void EditorHost::drainPropertyCommandQueue_() {
    m_commandStack.pendingQueue().drain();
    for (const EditorCommand& command : m_commandStack.pendingQueue().lastDrainedBatch()) {
        applyCommand_(command);
        ++m_commandsAppliedLastTick;
    }
}

void EditorHost::undoPropertyEdit() {
    ensureInitialized_();
    m_commandStack.undo();
    drainPropertyCommandQueue_();
}

void EditorHost::redoPropertyEdit() {
    ensureInitialized_();
    m_commandStack.redo();
    drainPropertyCommandQueue_();
}

void EditorHost::gameTick() {
    ensureInitialized_();

    m_commandsAppliedLastTick = 0;
    m_queue.drain();
    for (const EditorCommand& command : m_queue.lastDrainedBatch()) {
        if (isUndoableEntityProperty(command)) {
            EditorCommand stacked = command;
            const std::string before = command.propertyValueBefore.empty()
                                           ? capturePropertyValueBefore(*this, command)
                                           : command.propertyValueBefore;
            m_commandStack.push(std::move(stacked), before);
        } else {
            applyCommand_(command);
        }
        ++m_commandsAppliedLastTick;
    }

    drainPropertyCommandQueue_();

    m_runtimeViewport.tick(*this, kEditorTickDt);

    if (m_state.playing && !m_state.paused) {
        syncPieAiBindings_();
        m_aiTreeFileWatch.pollOsFileChanges(m_pieBehaviorRuntime);
        m_playSession.tick(kEditorTickDt, m_editorScene, m_physics);
    }

    ++m_gameTickCount;
}

} // namespace fuse::editor
