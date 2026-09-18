#pragma once

#include <fuse/editor/command_queue.hpp>

#include <string_view>
#include <vector>

namespace fuse::editor {

/// Captured `CommandStack` state for snapshot/restore stubs (B6.2 deepen).
struct CommandStackSnapshot {
    std::vector<EditorCommand> undoStack;
    std::vector<EditorCommand> redoStack;
    u32 undoDepth = 0;
    u32 redoDepth = 0;
    u32 appliedCount = 0;
    u32 coalescedCount = 0;
    u32 coalescedCountAtBaseline = 0;
    u32 baselineUndoDepth = 0;
    u32 baselineRedoDepth = 0;
    bool baselineConfigured = false;
    u32 evictedCount = 0;
    bool dirty = false;
    u32 dirtyRevision = 0;
};

/// Undo/redo stack for `EditorCommand` envelopes — records panel edit history for
/// headless tests (B6.2 follow-up, B6.6–B6.8 panel stubs).
class CommandStack {
public:
    static constexpr u32 kMaxHistory = 256;

    void execute(EditorCommand command);
    void push(EditorCommand command) { execute(std::move(command)); }
    void push(EditorCommand command, std::string_view beforeValue);
    void undo();
    void redo();
    void clear();

    [[nodiscard]] bool canUndo() const { return m_undoDepth > 0u; }
    [[nodiscard]] bool canRedo() const { return m_redoDepth > 0u; }
    /// True when the undo branch has no recorded steps (B6.2 deepen — empty-stack early-out).
    [[nodiscard]] bool isEmpty() const { return m_undoDepth == 0u && m_undoStack.empty(); }
    /// True after `set_baseline_state` has recorded a saved-document depth (B6.2 deepen).
    [[nodiscard]] bool isBaselineConfigured() const { return m_baselineConfigured; }

    u32 undoDepth() const { return m_undoDepth; }
    u32 redoDepth() const { return m_redoDepth; }
    u32 appliedCount() const { return m_appliedCount; }
    u32 coalescedCount() const { return m_coalescedCount; }
    /// Coalesce events recorded since the last `set_baseline_state` call.
    u32 coalescedCountSinceBaseline() const;
    u32 evictedCount() const { return m_evictedCount; }

    [[nodiscard]] bool isDirty() const { return m_dirty; }
    u32 dirtyRevision() const { return m_dirtyRevision; }
    /// True when `dirtyRevision()` has advanced past `revision` (B6.2 deepen).
    [[nodiscard]] bool isDirtySince(u32 revision) const { return dirtyRevision() > revision; }
    void markClean();
    /// Records the current undo/redo depth as the saved-document baseline (B6.2 deepen).
    void set_baseline_state();
    [[nodiscard]] bool isAtBaseline() const;
    /// True when undo/redo depth differs from the last `set_baseline_state` call.
    [[nodiscard]] bool hasUnsavedChanges() const { return !isAtBaseline(); }

    CommandStackSnapshot captureSnapshot() const;
    void restoreSnapshot(const CommandStackSnapshot& snapshot);

    const EditorCommand* lastApplied() const;
    const EditorCommand* peekUndo() const;
    const EditorCommand* peekRedo() const;
    CommandQueue& pendingQueue() { return m_pending; }
    const CommandQueue& pendingQueue() const { return m_pending; }

private:
    static bool canCoalesce_(const EditorCommand& previous, const EditorCommand& incoming);

    void evictOldestIfNeeded_();
    void markDirty_();
    void markDirtyAndBump_();
    /// Clears dirty when undo/redo depth matches the saved baseline; otherwise marks dirty.
    void syncBaselineDirty_();

    CommandQueue m_pending;
    std::vector<EditorCommand> m_undoStack;
    std::vector<EditorCommand> m_redoStack;
    u32 m_undoDepth = 0;
    u32 m_redoDepth = 0;
    u32 m_appliedCount = 0;
    u32 m_coalescedCount = 0;
    u32 m_coalescedCountAtBaseline = 0;
    u32 m_baselineUndoDepth = 0;
    u32 m_baselineRedoDepth = 0;
    bool m_baselineConfigured = false;
    u32 m_evictedCount = 0;
    bool m_dirty = false;
    u32 m_dirtyRevision = 0;
};

} // namespace fuse::editor
