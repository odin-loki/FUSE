#pragma once

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

/// Qt-free in-process editor runtime host (WP-08 / U6).
/// UI thread calls postFromUi(); game thread calls gameTick() to drain commands.
class EditorHost {
public:
    CommandQueue& commandQueue() { return m_queue; }
    const CommandQueue& commandQueue() const { return m_queue; }

    UndoStack& undoStack() { return m_undoStack; }
    const UndoStack& undoStack() const { return m_undoStack; }

    void postFromUi(EditorCommand command);
    void gameTick();

    u32 gameTickCount() const { return m_gameTickCount; }

private:
    CommandQueue m_queue;
    UndoStack m_undoStack;
    u32 m_gameTickCount = 0;
};

} // namespace fuse::editor
