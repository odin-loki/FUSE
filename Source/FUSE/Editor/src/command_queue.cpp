#include <fuse/editor/command_queue.hpp>

namespace fuse::editor {

void CommandQueue::post(EditorCommand /*command*/) {
    ++m_pending;
}

void CommandQueue::drain() {
    m_applied += m_pending;
    m_pending = 0;
}

} // namespace fuse::editor
