#pragma once

#include <fuse/editor/command_queue.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

/// Qt-free in-process editor runtime host (WP-08 / U6).
/// UI thread calls postFromUi(); game thread calls gameTick() to drain commands.
class EditorHost {
public:
    CommandQueue& commandQueue() { return m_queue; }
    const CommandQueue& commandQueue() const { return m_queue; }

    void postFromUi(EditorCommand command);
    void gameTick();

    u32 gameTickCount() const { return m_gameTickCount; }

private:
    CommandQueue m_queue;
    u32 m_gameTickCount = 0;
};

} // namespace fuse::editor
