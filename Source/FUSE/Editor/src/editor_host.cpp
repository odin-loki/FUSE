#include <fuse/editor/editor_host.hpp>

#include <fuse/ecs/components/transform.hpp>

namespace fuse::editor {

namespace {

constexpr f32 kEditorTickDt = 1.f / 60.f;

} // namespace

EditorHost::EditorHost() = default;

EditorHost::~EditorHost() {
    if (m_initialized) {
        m_editorScene.destroy();
    }
}

EditorScene& EditorHost::editorScene() {
    ensureInitialized_();
    return m_editorScene;
}

const EditorScene& EditorHost::editorScene() const {
    return const_cast<EditorHost*>(this)->editorScene();
}

void EditorHost::ensureInitialized_() {
    if (m_initialized) {
        return;
    }

    m_editorScene.init();
    m_runtimeScene = scene::Scene("EditorHostScene");
    m_initialized = true;
}

void EditorHost::postFromUi(EditorCommand command) {
    m_queue.post(std::move(command));
}

void EditorHost::applyCommand_(const EditorCommand& command) {
    ensureInitialized_();

    switch (command.kind) {
    case CommandKind::SetProperty:
        if (command.propertyName == "project") {
            m_loadedProject = command.propertyValue;
        }
        break;
    case CommandKind::SelectEntity:
        if (command.target.isValid()) {
            ecs::EntityID selected{};
            selected.index = command.target.index();
            selected.generation = command.target.generation();
            m_state.primarySelection = selected;
            m_state.selectedEntities.clear();
            if (m_state.primarySelection.valid()) {
                m_state.selectedEntities.push_back(m_state.primarySelection);
            }
        }
        break;
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
    case CommandKind::DeleteObject:
    case CommandKind::ReparentObject:
        break;
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

    if (m_state.playing && !m_state.paused) {
        m_playSession.tick(kEditorTickDt, m_editorScene, m_physics);
    }

    ++m_gameTickCount;
}

} // namespace fuse::editor
