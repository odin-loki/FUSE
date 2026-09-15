#pragma once

#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::editor {

/// Captured `UndoStack` metadata for snapshot/restore (B6.2 deepen).
/// Restore rewinds live undo/redo depth; command cloning remains deferred.
struct UndoStackSnapshot {
    u32 undoCount = 0;
    u32 redoCount = 0;
    u32 evictedCount = 0;
    u32 coalescedOps = 0;
    u32 coalescedOpsAtBaseline = 0;
    u32 baselineUndoCount = 0;
    u32 baselineRedoCount = 0;
    u32 dirtyRevisionAtBaseline = 0;
    bool baselineConfigured = false;
    bool dirty = false;
    u32 dirtyRevision = 0;
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
    void push(std::unique_ptr<UndoCommand> command) { execute(std::move(command)); }
    void undo();
    void redo();

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    [[nodiscard]] bool isEmpty() const { return m_undo.empty() && m_redo.empty(); }

    u32 undoCount() const { return static_cast<u32>(m_undo.size()); }
    u32 redoCount() const { return static_cast<u32>(m_redo.size()); }
    u32 evictedCount() const { return m_evictedCount; }
    u32 coalescedOps() const { return m_coalescedOps; }
    /// Coalesce events recorded since the last `set_baseline_state` call.
    u32 coalescedOpsSinceBaseline() const;

    [[nodiscard]] bool isDirty() const { return m_dirty; }
    u32 dirtyRevision() const { return m_dirtyRevision; }
    /// Dirty revision recorded at the last `set_baseline_state` call.
    u32 dirtyRevisionAtBaseline() const { return m_dirtyRevisionAtBaseline; }
    /// True when empty-stack undo/redo/null push will not advance `dirtyRevision`.
    [[nodiscard]] bool isDirtyRevisionStable() const;
    void markClean();
    /// Records the current undo/redo depth as the saved-document baseline (B6.2 deepen).
    void set_baseline_state();
    [[nodiscard]] bool isAtBaseline() const;

    /// True when undo/redo depth differs from the last `set_baseline_state` call.
    [[nodiscard]] bool hasUnsavedChanges() const { return !isAtBaseline(); }

    std::string peekUndoDescription() const;
    std::string peekRedoDescription() const;

    /// Probe whether the next `execute` would merge into the top undo entry.
    [[nodiscard]] bool wouldCoalesceWith(const UndoCommand& command) const;

    void clear();

    UndoStackSnapshot captureSnapshot() const;
    void restoreSnapshot(const UndoStackSnapshot& snapshot);

private:
    void evictOldestIfNeeded_();

    void markDirty_();
    /// Clears dirty when undo depth matches the saved baseline; otherwise marks dirty.
    void syncBaselineDirty_();

    std::vector<std::unique_ptr<UndoCommand>> m_undo;
    std::vector<std::unique_ptr<UndoCommand>> m_redo;
    u32 m_evictedCount = 0;
    u32 m_coalescedOps = 0;
    u32 m_coalescedOpsAtBaseline = 0;
    u32 m_baselineUndoCount = 0;
    u32 m_baselineRedoCount = 0;
    u32 m_dirtyRevisionAtBaseline = 0;
    bool m_baselineConfigured = false;
    bool m_dirty = false;
    u32 m_dirtyRevision = 0;
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
