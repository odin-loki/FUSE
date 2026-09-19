#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::editor {

enum class CommandKind {
    SetProperty,
    DeleteObject,
    ReparentObject,
    SelectEntity,
    StartPlay,
    StopPlay,
    PausePlay,
    ResumePlay,
    Undo,
    Redo,
};

/// UI-thread command envelope — applied on the game thread via CommandQueue::drain().
/// Full Qt editor panes land in U6; this header stays Qt-free.
struct EditorCommand {
    CommandKind kind = CommandKind::SetProperty;
    Handle<Object> target = Handle<Object>::invalid();
    Handle<Object> parent = Handle<Object>::invalid();
    std::string propertyName;
    std::string propertyValue;
    /// First value in a coalesced drag group — preserved when later edits merge (B6.2 deepen).
    std::string propertyValueBefore;
};

/// Thread-safe queue: post from UI thread, drain on game thread.
class CommandQueue {
public:
    void post(EditorCommand command);
    void drain();
    u32 pendingCount() const;
    u32 appliedCount() const { return m_applied; }

    /// Commands moved out of the pending queue by the most recent drain() call.
    const std::vector<EditorCommand>& lastDrainedBatch() const { return m_lastDrained; }

private:
    mutable std::mutex m_mutex;
    std::deque<EditorCommand> m_pendingDeque;
    std::vector<EditorCommand> m_lastDrained;
    u32 m_pending = 0;
    u32 m_applied = 0;
};

} // namespace fuse::editor
