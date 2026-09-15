#include <fuse/editor/command_stack.hpp>

namespace fuse::editor {

void CommandStack::execute(EditorCommand command) {
    m_undoStack.push_back(std::move(command));
    m_redoStack.clear();
    ++m_undoDepth;
    m_redoDepth = 0;

    m_pending.post(m_undoStack.back());
    ++m_appliedCount;
}

void CommandStack::undo() {
    if (m_undoDepth == 0u || m_undoStack.empty()) {
        return;
    }

    m_redoStack.push_back(std::move(m_undoStack.back()));
    m_undoStack.pop_back();
    --m_undoDepth;
    ++m_redoDepth;
}

void CommandStack::redo() {
    if (m_redoDepth == 0u || m_redoStack.empty()) {
        return;
    }

    m_undoStack.push_back(std::move(m_redoStack.back()));
    m_redoStack.pop_back();
    ++m_undoDepth;
    --m_redoDepth;

    m_pending.post(m_undoStack.back());
    ++m_appliedCount;
}

const EditorCommand* CommandStack::lastApplied() const {
    if (m_undoStack.empty()) {
        return nullptr;
    }
    return &m_undoStack.back();
}

} // namespace fuse::editor
