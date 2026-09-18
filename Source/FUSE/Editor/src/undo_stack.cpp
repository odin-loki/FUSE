#include <fuse/editor/undo_stack.hpp>

namespace fuse::editor {

namespace {

const std::string kEmptyDescription;

} // namespace

void UndoStack::evictOldestIfNeeded_() {
    if (m_undo.size() <= kMaxHistory) {
        return;
    }

    m_undo.erase(m_undo.begin());
    ++m_evictedCount;
    if (m_baselineConfigured && m_baselineUndoCount > 0u) {
        --m_baselineUndoCount;
    }
}

void UndoStack::markDirty_() {
    if (!m_dirty) {
        ++m_dirtyRevision;
    }
    m_dirty = true;
}

void UndoStack::markDirtyAndBump_() {
    m_dirty = true;
    ++m_dirtyRevision;
}

void UndoStack::syncBaselineDirty_() {
    if (!m_baselineConfigured) {
        markDirtyAndBump_();
        return;
    }

    if (isAtBaseline() && coalescedOpsSinceBaseline() == 0u) {
        markClean();
        return;
    }

    markDirtyAndBump_();
}

u32 UndoStack::coalescedOpsSinceBaseline() const {
    if (m_coalescedOps < m_coalescedOpsAtBaseline) {
        return 0u;
    }

    return m_coalescedOps - m_coalescedOpsAtBaseline;
}

void UndoStack::execute(std::unique_ptr<UndoCommand> command) {
    if (!command) {
        return;
    }

    if (!m_undo.empty() && canUndo() && m_undo.back()->merge(*command)) {
        m_undo.back()->execute();
        ++m_coalescedOps;
        markDirty_();
        return;
    }

    command->execute();
    m_undo.push_back(std::move(command));
    m_redo.clear();
    evictOldestIfNeeded_();
    markDirtyAndBump_();
}

void UndoStack::set_baseline_state() {
    if (m_baselineConfigured && isAtBaseline() && m_coalescedOpsAtBaseline == m_coalescedOps &&
        m_baselineRedoCount == redoCount()) {
        markClean();
        return;
    }

    m_baselineUndoCount = undoCount();
    m_baselineRedoCount = redoCount();
    m_coalescedOpsAtBaseline = m_coalescedOps;
    m_baselineConfigured = true;
    markClean();
}

bool UndoStack::isAtBaseline() const {
    return undoCount() == m_baselineUndoCount;
}

void UndoStack::undo() {
    if (m_undo.empty()) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->undo();
    m_redo.push_back(std::move(command));
    syncBaselineDirty_();
}

void UndoStack::redo() {
    if (m_redo.empty()) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_redo.back());
    m_redo.pop_back();
    command->execute();
    m_undo.push_back(std::move(command));
    evictOldestIfNeeded_();
    syncBaselineDirty_();
}

std::string UndoStack::peekUndoDescription() const {
    if (m_undo.empty()) {
        return kEmptyDescription;
    }
    return m_undo.back()->description();
}

std::string UndoStack::peekRedoDescription() const {
    if (m_redo.empty()) {
        return kEmptyDescription;
    }
    return m_redo.back()->description();
}

void UndoStack::clear() {
    if (isEmpty() && m_redo.empty() && !m_baselineConfigured && m_coalescedOps == 0u && !m_dirty &&
        m_dirtyRevision == 0u) {
        return;
    }

    m_undo.clear();
    m_redo.clear();
    m_evictedCount = 0;
    m_coalescedOps = 0;
    m_coalescedOpsAtBaseline = 0;
    m_baselineUndoCount = 0;
    m_baselineRedoCount = 0;
    m_baselineConfigured = false;
    m_dirty = false;
    m_dirtyRevision = 0;
}

void UndoStack::markClean() {
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
}

UndoStackSnapshot UndoStack::captureSnapshot() const {
    UndoStackSnapshot snapshot;
    snapshot.undoCount = undoCount();
    snapshot.redoCount = redoCount();
    snapshot.evictedCount = m_evictedCount;
    snapshot.coalescedOps = m_coalescedOps;
    snapshot.coalescedOpsAtBaseline = m_coalescedOpsAtBaseline;
    snapshot.baselineUndoCount = m_baselineUndoCount;
    snapshot.baselineRedoCount = m_baselineRedoCount;
    snapshot.baselineConfigured = m_baselineConfigured;
    snapshot.dirty = m_dirty;
    snapshot.dirtyRevision = m_dirtyRevision;
    snapshot.undoDescriptions.reserve(m_undo.size());
    for (const auto& command : m_undo) {
        snapshot.undoDescriptions.push_back(command->description());
    }
    snapshot.redoDescriptions.reserve(m_redo.size());
    for (const auto& command : m_redo) {
        snapshot.redoDescriptions.push_back(command->description());
    }
    return snapshot;
}

void UndoStack::restoreSnapshot(const UndoStackSnapshot& snapshot) {
    while (undoCount() < snapshot.undoCount && canRedo()) {
        redo();
    }

    while (undoCount() > snapshot.undoCount && canUndo()) {
        undo();
    }

    while (redoCount() > snapshot.redoCount) {
        m_redo.pop_back();
    }

    m_evictedCount = snapshot.evictedCount;
    m_coalescedOps = snapshot.coalescedOps;
    m_coalescedOpsAtBaseline = snapshot.coalescedOpsAtBaseline;
    m_baselineUndoCount = snapshot.baselineUndoCount;
    m_baselineRedoCount = snapshot.baselineRedoCount;
    m_baselineConfigured = snapshot.baselineConfigured;
    m_dirty = snapshot.dirty;
    m_dirtyRevision = snapshot.dirtyRevision;
}

SetObjectNameCommand::SetObjectNameCommand(Object& object, std::string before, std::string after)
    : m_object(object), m_before(std::move(before)), m_after(std::move(after)) {}

void SetObjectNameCommand::execute() {
    m_object.setName(m_after);
}

void SetObjectNameCommand::undo() {
    m_object.setName(m_before);
}

std::string SetObjectNameCommand::description() const {
    return "Rename " + m_before + " to " + m_after;
}

ReparentObjectCommand::ReparentObjectCommand(Object& object, Object* newParent, Object* oldParent)
    : m_object(object), m_newParent(newParent), m_oldParent(oldParent) {}

void ReparentObjectCommand::execute() {
    if (m_newParent) {
        m_newParent->addChild(&m_object);
        return;
    }
    if (m_oldParent) {
        m_oldParent->removeChild(&m_object);
    }
}

void ReparentObjectCommand::undo() {
    if (m_newParent) {
        m_newParent->removeChild(&m_object);
    }
    if (m_oldParent) {
        m_oldParent->addChild(&m_object);
    }
}

std::string ReparentObjectCommand::description() const {
    return "Reparent " + m_object.name();
}

} // namespace fuse::editor
