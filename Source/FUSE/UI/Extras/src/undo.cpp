#include "fuse/ui/extras/undo.hpp"

#include <algorithm>

namespace fuse::ui {

namespace {
class MacroCommand final : public UndoCommand {
public:
    using UndoCommand::UndoCommand;
    void redo() override { for (auto& c : kids) c->redo(); }
    void undo() override { for (auto it = kids.rbegin(); it != kids.rend(); ++it) (*it)->undo(); }
    std::vector<std::unique_ptr<UndoCommand>> kids;
};
} // namespace

bool FnCommand::mergeWith(const UndoCommand& n) {
    if (!m_merge) return false;
    const auto* o = dynamic_cast<const FnCommand*>(&n);
    return o && m_merge(*this, *o);
}

UndoStack::~UndoStack() = default;

void UndoStack::push(std::string text, std::function<void()> r, std::function<void()> u, int id) {
    push(std::make_unique<FnCommand>(std::move(text), std::move(r), std::move(u), id));
}

void UndoStack::push(std::unique_ptr<UndoCommand> cmd) {
    if (!cmd) return;
    cmd->redo();
    if (!m_macros.empty()) {
        auto& kids = static_cast<MacroCommand&>(*m_macros.back()).kids;
        if (!kids.empty() && cmd->id() >= 0 && kids.back()->id() == cmd->id() && kids.back()->mergeWith(*cmd)) return;
        kids.push_back(std::move(cmd));
        return;
    }
    // drop the redo tail; a clean state inside it becomes unreachable
    if (m_cleanIndex > m_index) m_cleanIndex = -1;
    m_cmds.erase(m_cmds.begin() + m_index, m_cmds.end());
    UndoCommand* top = m_index > 0 ? m_cmds[m_index - 1].get() : nullptr;
    const bool canMerge = top && cmd->id() >= 0 && top->id() == cmd->id() && m_cleanIndex != m_index;
    if (canMerge && top->mergeWith(*cmd)) {
        if (top->obsolete()) { m_cmds.pop_back(); --m_index; }
        changed();
        return;
    }
    if (cmd->obsolete()) { changed(); return; }
    m_cmds.push_back(std::move(cmd));
    ++m_index;
    if (m_limit > 0 && int(m_cmds.size()) > m_limit) {
        const int drop = int(m_cmds.size()) - m_limit;
        m_cmds.erase(m_cmds.begin(), m_cmds.begin() + drop);
        m_index -= drop;
        m_cleanIndex = m_cleanIndex >= drop ? m_cleanIndex - drop : -1;
    }
    changed();
}

void UndoStack::undo() {
    if (!canUndo()) return;
    UndoCommand& c = *m_cmds[--m_index];
    c.undo();
    if (c.obsolete()) {
        m_cmds.erase(m_cmds.begin() + m_index);
        if (m_cleanIndex > m_index) --m_cleanIndex;
    }
    changed();
}
void UndoStack::redo() {
    if (!canRedo()) return;
    UndoCommand& c = *m_cmds[m_index];
    c.redo();
    if (c.obsolete()) { m_cmds.erase(m_cmds.begin() + m_index); if (m_cleanIndex > m_index) --m_cleanIndex; }
    else ++m_index;
    changed();
}
void UndoStack::setIndex(int idx) {
    idx = std::clamp(idx, 0, count());
    while (m_index > idx && canUndo()) undo();
    while (m_index < idx && canRedo()) redo();
}
bool UndoStack::canUndo() const { return m_macros.empty() && m_index > 0; }
bool UndoStack::canRedo() const { return m_macros.empty() && m_index < count(); }
std::string UndoStack::undoText() const { return canUndo() ? m_cmds[m_index - 1]->text() : std::string(); }
std::string UndoStack::redoText() const { return canRedo() ? m_cmds[m_index]->text() : std::string(); }
const UndoCommand* UndoStack::command(int i) const { return i >= 0 && i < count() ? m_cmds[i].get() : nullptr; }

void UndoStack::beginMacro(std::string text) { m_macros.push_back(std::make_unique<MacroCommand>(std::move(text))); }
void UndoStack::endMacro() {
    if (m_macros.empty()) return;
    std::unique_ptr<UndoCommand> m = std::move(m_macros.back());
    m_macros.pop_back();
    if (static_cast<MacroCommand&>(*m).kids.empty()) return;
    // children already ran; record without re-running
    if (!m_macros.empty()) { static_cast<MacroCommand&>(*m_macros.back()).kids.push_back(std::move(m)); return; }
    if (m_cleanIndex > m_index) m_cleanIndex = -1;
    m_cmds.erase(m_cmds.begin() + m_index, m_cmds.end());
    m_cmds.push_back(std::move(m));
    ++m_index;
    changed();
}
void UndoStack::setClean() { m_cleanIndex = m_index; changed(); }
void UndoStack::resetClean() { m_cleanIndex = -1; changed(); }
bool UndoStack::setUndoLimit(int limit) {
    if (!m_cmds.empty() || limit < 0) return false;
    m_limit = limit;
    return true;
}
void UndoStack::clear() {
    m_cmds.clear();
    m_macros.clear();
    m_index = m_cleanIndex = 0;
    changed();
}

void UndoGroup::addStack(UndoStack* s) { if (s && std::find(m_stacks.begin(), m_stacks.end(), s) == m_stacks.end()) m_stacks.push_back(s); }
void UndoGroup::removeStack(UndoStack* s) {
    std::erase(m_stacks, s);
    if (m_active == s) m_active = nullptr;
}
void UndoGroup::setActiveStack(UndoStack* s) { addStack(s); m_active = s; }

} // namespace fuse::ui
