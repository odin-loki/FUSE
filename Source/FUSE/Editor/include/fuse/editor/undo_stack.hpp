#pragma once

#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::editor {

/// Captured `UndoStack` metadata for snapshot/restore stubs (B6.2 deepen).
/// Command payloads remain live on the stack; restore rewinds depth counters only.
struct UndoStackSnapshot {
    u32 undoCount = 0;
    u32 redoCount = 0;
    u32 evictedCount = 0;
    std::vector<std::string> undoDescriptions;
    std::vector<std::string> redoDescriptions;
};

/// Undo/redo command interface (B6.2). Game-thread mutations only.
class UndoCommand {
public:
    virtual ~UndoCommand() = default;

    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::string description() const = 0;

    /// Merge consecutive commands (e.g. transform drags). Returns true when merged.
    virtual bool merge(const UndoCommand& other) { return false; }
};

/// LIFO undo/redo stack for reversible scene mutations (B6.2).
class UndoStack {
public:
    static constexpr u32 kMaxHistory = 256;

    void execute(std::unique_ptr<UndoCommand> command);
    void undo();
    void redo();

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    u32 undoCount() const { return static_cast<u32>(m_undo.size()); }
    u32 redoCount() const { return static_cast<u32>(m_redo.size()); }
    u32 evictedCount() const { return m_evictedCount; }

    std::string peekUndoDescription() const;
    std::string peekRedoDescription() const;

    void clear();

    UndoStackSnapshot captureSnapshot() const;
    void restoreSnapshot(const UndoStackSnapshot& snapshot);

private:
    void evictOldestIfNeeded_();

    std::vector<std::unique_ptr<UndoCommand>> m_undo;
    std::vector<std::unique_ptr<UndoCommand>> m_redo;
    u32 m_evictedCount = 0;
};

/// Rename an object and restore the previous name on undo.
class SetObjectNameCommand final : public UndoCommand {
public:
    SetObjectNameCommand(Object& object, std::string before, std::string after);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    Object& m_object;
    std::string m_before;
    std::string m_after;
};

/// Reparent an object in the scene graph.
class ReparentObjectCommand final : public UndoCommand {
public:
    ReparentObjectCommand(Object& object, Object* newParent, Object* oldParent);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    Object& m_object;
    Object* m_newParent = nullptr;
    Object* m_oldParent = nullptr;
};

} // namespace fuse::editor
