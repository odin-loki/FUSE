#pragma once

#include <fuse/editor/command_queue.hpp>

#include <vector>

namespace fuse::editor {

/// Minimal undo/redo stack stub — records `EditorCommand` history for headless tests (B6.2 follow-up).
class CommandStack {
public:
    void execute(EditorCommand command);
    void undo();
    void redo();

    u32 undoDepth() const { return m_undoDepth; }
    u32 redoDepth() const { return m_redoDepth; }
    u32 appliedCount() const { return m_appliedCount; }

    const EditorCommand* lastApplied() const;
    CommandQueue& pendingQueue() { return m_pending; }
    const CommandQueue& pendingQueue() const { return m_pending; }

private:
    CommandQueue m_pending;
    std::vector<EditorCommand> m_undoStack;
    std::vector<EditorCommand> m_redoStack;
    u32 m_undoDepth = 0;
    u32 m_redoDepth = 0;
    u32 m_appliedCount = 0;
};

} // namespace fuse::editor
