#include <fuse/editor/command_stack.hpp>

namespace fuse::editor {

bool CommandStack::canCoalesce_(const EditorCommand& previous, const EditorCommand& incoming) {
    if (previous.kind != incoming.kind || previous.kind != CommandKind::SetProperty) {
        return false;
    }

    return previous.target == incoming.target && previous.propertyName == incoming.propertyName;
}

void CommandStack::evictOldestIfNeeded_() {
    if (m_undoStack.size() <= kMaxHistory) {
        return;
    }

    m_undoStack.erase(m_undoStack.begin());
    ++m_evictedCount;
    if (m_undoDepth > 0u) {
        --m_undoDepth;
    }
    if (m_baselineConfigured && m_baselineUndoDepth > 0u) {
        --m_baselineUndoDepth;
    }
}

void CommandStack::markDirty_() {
    if (!m_dirty) {
        ++m_dirtyRevision;
    }
    m_dirty = true;
}

void CommandStack::markDirtyAndBump_() {
    m_dirty = true;
    ++m_dirtyRevision;
}

void CommandStack::syncBaselineDirty_() {
    if (!m_baselineConfigured) {
        markDirtyAndBump_();
        return;
    }

    if (isAtBaseline()) {
        markClean();
        return;
    }

    markDirtyAndBump_();
}

u32 CommandStack::coalescedCountSinceBaseline() const {
    if (m_coalescedCount < m_coalescedCountAtBaseline) {
        return 0u;
    }

    return m_coalescedCount - m_coalescedCountAtBaseline;
}

void CommandStack::set_baseline_state() {
    if (m_baselineConfigured && isAtBaseline() && m_coalescedCountAtBaseline == m_coalescedCount &&
        m_baselineRedoDepth == m_redoDepth) {
        markClean();
        return;
    }

    m_baselineUndoDepth = m_undoDepth;
    m_baselineRedoDepth = m_redoDepth;
    m_coalescedCountAtBaseline = m_coalescedCount;
    m_baselineConfigured = true;
    markClean();
}

bool CommandStack::isAtBaseline() const {
    return m_undoDepth == m_baselineUndoDepth;
}

void CommandStack::execute(EditorCommand command) {
    if (m_undoDepth > 0u && !m_undoStack.empty() && canCoalesce_(m_undoStack.back(), command)) {
        m_undoStack.back().propertyValue = command.propertyValue;
        ++m_coalescedCount;
        markDirty_();
        m_pending.post(m_undoStack.back());
        ++m_appliedCount;
        return;
    }

    if (command.propertyValueBefore.empty()) {
        command.propertyValueBefore = command.propertyValue;
    }
    m_undoStack.push_back(std::move(command));
    m_redoStack.clear();
    ++m_undoDepth;
    m_redoDepth = 0;
    evictOldestIfNeeded_();
    markDirtyAndBump_();

    m_pending.post(m_undoStack.back());
    ++m_appliedCount;
}

void CommandStack::push(EditorCommand command, std::string_view beforeValue) {
    command.propertyValueBefore = beforeValue;
    execute(std::move(command));
}

void CommandStack::undo() {
    if (m_undoDepth == 0u || m_undoStack.empty()) {
        return;
    }

    m_redoStack.push_back(std::move(m_undoStack.back()));
    m_undoStack.pop_back();
    --m_undoDepth;
    ++m_redoDepth;
    syncBaselineDirty_();

    const EditorCommand& undone = m_redoStack.back();
    if (undone.kind == CommandKind::SetProperty) {
        EditorCommand inverse;
        inverse.kind = CommandKind::SetProperty;
        inverse.target = undone.target;
        inverse.propertyName = undone.propertyName;
        inverse.propertyValue = undone.propertyValueBefore;
        inverse.propertyValueBefore = undone.propertyValue;
        m_pending.post(inverse);
        ++m_appliedCount;
    }
}

void CommandStack::redo() {
    if (m_redoDepth == 0u || m_redoStack.empty()) {
        return;
    }

    m_undoStack.push_back(std::move(m_redoStack.back()));
    m_redoStack.pop_back();
    ++m_undoDepth;
    --m_redoDepth;
    evictOldestIfNeeded_();
    syncBaselineDirty_();

    m_pending.post(m_undoStack.back());
    ++m_appliedCount;
}

void CommandStack::clear() {
    m_undoStack.clear();
    m_redoStack.clear();
    m_undoDepth = 0;
    m_redoDepth = 0;
    m_appliedCount = 0;
    m_coalescedCount = 0;
    m_coalescedCountAtBaseline = 0;
    m_baselineUndoDepth = 0;
    m_baselineRedoDepth = 0;
    m_baselineConfigured = false;
    m_evictedCount = 0;
    m_dirty = false;
    m_dirtyRevision = 0;
}

void CommandStack::markClean() {
    m_dirty = false;
}

CommandStackSnapshot CommandStack::captureSnapshot() const {
    CommandStackSnapshot snapshot;
    snapshot.undoStack = m_undoStack;
    snapshot.redoStack = m_redoStack;
    snapshot.undoDepth = m_undoDepth;
    snapshot.redoDepth = m_redoDepth;
    snapshot.appliedCount = m_appliedCount;
    snapshot.coalescedCount = m_coalescedCount;
    snapshot.coalescedCountAtBaseline = m_coalescedCountAtBaseline;
    snapshot.baselineUndoDepth = m_baselineUndoDepth;
    snapshot.baselineRedoDepth = m_baselineRedoDepth;
    snapshot.baselineConfigured = m_baselineConfigured;
    snapshot.evictedCount = m_evictedCount;
    snapshot.dirty = m_dirty;
    snapshot.dirtyRevision = m_dirtyRevision;
    return snapshot;
}

void CommandStack::restoreSnapshot(const CommandStackSnapshot& snapshot) {
    m_undoStack = snapshot.undoStack;
    m_redoStack = snapshot.redoStack;
    m_undoDepth = snapshot.undoDepth;
    m_redoDepth = snapshot.redoDepth;
    m_appliedCount = snapshot.appliedCount;
    m_coalescedCount = snapshot.coalescedCount;
    m_coalescedCountAtBaseline = snapshot.coalescedCountAtBaseline;
    m_baselineUndoDepth = snapshot.baselineUndoDepth;
    m_baselineRedoDepth = snapshot.baselineRedoDepth;
    m_baselineConfigured = snapshot.baselineConfigured;
    m_evictedCount = snapshot.evictedCount;
    m_dirty = snapshot.dirty;
    m_dirtyRevision = snapshot.dirtyRevision;
}

const EditorCommand* CommandStack::lastApplied() const {
    if (m_undoStack.empty()) {
        return nullptr;
    }
    return &m_undoStack.back();
}

const EditorCommand* CommandStack::peekUndo() const {
    if (m_undoDepth == 0u || m_undoStack.empty()) {
        return nullptr;
    }
    return &m_undoStack.back();
}

const EditorCommand* CommandStack::peekRedo() const {
    if (m_redoDepth == 0u || m_redoStack.empty()) {
        return nullptr;
    }
    return &m_redoStack.back();
}

} // namespace fuse::editor
