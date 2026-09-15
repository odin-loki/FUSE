#include <fuse/editor/editor_host.hpp>

namespace fuse::editor {

void EditorHost::postFromUi(EditorCommand command) {
    m_queue.post(std::move(command));
}

void EditorHost::gameTick() {
    m_queue.drain();
    ++m_gameTickCount;
}

} // namespace fuse::editor
