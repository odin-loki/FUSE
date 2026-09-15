#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

enum class CommandKind {
    SetProperty,
    DeleteObject,
    ReparentObject,
};

/// UI-thread command envelope — applied on the game thread via CommandQueue::drain().
/// Full Qt editor panes land in U6; this header stays Qt-free.
struct EditorCommand {
    CommandKind kind = CommandKind::SetProperty;
    Handle<Object> target = Handle<Object>::invalid();
    Handle<Object> parent = Handle<Object>::invalid();
    std::string propertyName;
    std::string propertyValue;
};

/// Thread-safe-ish queue: post from UI thread, drain on game thread.
class CommandQueue {
public:
    void post(EditorCommand command);
    void drain();
    u32 pendingCount() const { return m_pending; }
    u32 appliedCount() const { return m_applied; }

private:
    u32 m_pending = 0;
    u32 m_applied = 0;
};

} // namespace fuse::editor
