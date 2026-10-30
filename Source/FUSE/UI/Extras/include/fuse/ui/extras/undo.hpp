// Undo framework (Qt QUndoCommand / QUndoStack / QUndoGroup parity): commands with merge (compression), macros,
// clean state, undo limit, obsolete commands and change notification.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

class UndoCommand {
public:
    explicit UndoCommand(std::string text = {}) : m_text(std::move(text)) {}
    virtual ~UndoCommand() = default;
    virtual void redo() {}
    virtual void undo() {}
    /// Commands with the same id >= 0 pushed back to back are offered to mergeWith() (typing, dragging).
    virtual int id() const { return -1; }
    /// Absorb `next` into this command; true when merged (next is then discarded).
    virtual bool mergeWith(const UndoCommand& next) { (void)next; return false; }
    /// An obsolete command (e.g. merged to a no-op) is dropped from the stack.
    bool obsolete() const { return m_obsolete; }
    void setObsolete(bool o) { m_obsolete = o; }
    const std::string& text() const { return m_text; }
    void setText(std::string t) { m_text = std::move(t); }

private:
    std::string m_text;
    bool m_obsolete = false;
};

/// Lambda command: `push("Rename", [&]{...}, [&]{...})`.
class FnCommand final : public UndoCommand {
public:
    FnCommand(std::string text, std::function<void()> redo, std::function<void()> undo, int id = -1,
              std::function<bool(FnCommand&, const FnCommand&)> merge = {})
        : UndoCommand(std::move(text)), m_redo(std::move(redo)), m_undo(std::move(undo)), m_id(id), m_merge(std::move(merge)) {}
    void redo() override { if (m_redo) m_redo(); }
    void undo() override { if (m_undo) m_undo(); }
    int id() const override { return m_id; }
    bool mergeWith(const UndoCommand& n) override;
    std::function<void()> m_redo, m_undo;

private:
    int m_id;
    std::function<bool(FnCommand&, const FnCommand&)> m_merge;
};

class UndoStack {
public:
    UndoStack() = default;
    UndoStack(const UndoStack&) = delete;
    UndoStack& operator=(const UndoStack&) = delete;
    ~UndoStack();

    /// Runs cmd->redo() and records it (discarding the redo tail, merging when possible, honouring the limit).
    void push(std::unique_ptr<UndoCommand> cmd);
    void push(std::string text, std::function<void()> redo, std::function<void()> undo, int id = -1);
    void undo();
    void redo();
    /// Undo / redo until index() == idx.
    void setIndex(int idx);
    bool canUndo() const;
    bool canRedo() const;
    std::string undoText() const;
    std::string redoText() const;
    int index() const { return m_index; }
    int count() const { return int(m_cmds.size()); }
    const UndoCommand* command(int i) const;
    /// Groups subsequent pushes into one command until endMacro() (nestable).
    void beginMacro(std::string text);
    void endMacro();
    bool inMacro() const { return !m_macros.empty(); }
    void setClean();
    void resetClean();
    bool isClean() const { return m_cleanIndex == m_index; }
    int cleanIndex() const { return m_cleanIndex; }
    /// Max commands kept (0 = unlimited); only settable on an empty stack, like Qt.
    bool setUndoLimit(int limit);
    int undoLimit() const { return m_limit; }
    void clear();
    /// Called after every change of index / count / clean state.
    std::function<void()> onChanged;

private:
    void changed() { if (onChanged) onChanged(); }
    std::vector<std::unique_ptr<UndoCommand>> m_cmds;
    std::vector<std::unique_ptr<UndoCommand>> m_macros;
    int m_index = 0, m_cleanIndex = 0, m_limit = 0;
};

/// One active stack among several (one per document).
class UndoGroup {
public:
    void addStack(UndoStack* s);
    void removeStack(UndoStack* s);
    void setActiveStack(UndoStack* s);
    UndoStack* activeStack() const { return m_active; }
    void undo() { if (m_active) m_active->undo(); }
    void redo() { if (m_active) m_active->redo(); }
    bool canUndo() const { return m_active && m_active->canUndo(); }
    bool canRedo() const { return m_active && m_active->canRedo(); }

private:
    std::vector<UndoStack*> m_stacks;
    UndoStack* m_active = nullptr;
};

} // namespace fuse::ui
