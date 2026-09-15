#include <fuse/editor/undo_stack.hpp>

namespace fuse::editor {

namespace {

const std::string kEmptyDescription;

} // namespace

void UndoStack::execute(std::unique_ptr<UndoCommand> command) {
    if (!command) {
        return;
    }

    if (!m_undo.empty() && m_undo.back()->merge(*command)) {
        m_undo.back()->execute();
        return;
    }

    command->execute();
    m_undo.push_back(std::move(command));
    m_redo.clear();
}

void UndoStack::undo() {
    if (m_undo.empty()) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->undo();
    m_redo.push_back(std::move(command));
}

void UndoStack::redo() {
    if (m_redo.empty()) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_redo.back());
    m_redo.pop_back();
    command->execute();
    m_undo.push_back(std::move(command));
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
    m_undo.clear();
    m_redo.clear();
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
