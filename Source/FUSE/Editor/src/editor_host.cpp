#include <fuse/editor/editor_host.hpp>

namespace fuse::editor {

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

} // namespace fuse::editor
