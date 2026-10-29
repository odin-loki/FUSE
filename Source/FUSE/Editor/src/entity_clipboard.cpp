#include <fuse/editor/entity_clipboard.hpp>

#include <algorithm>

namespace fuse::editor {

namespace {
bool contains(const std::vector<ecs::EntityID>& v, ecs::EntityID id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}
} // namespace

usize EntityClipboard::copy(const ecs::Registry& registry, const std::vector<ecs::EntityID>& selection) {
    std::vector<ecs::EntityID> roots;
    for (ecs::EntityID id : selection) {
        if (!registry.alive(id) || contains(roots, id)) continue;
        // Skip entities whose ancestor is also selected (they come along as descendants).
        bool nested = false;
        ecs::EntityID walk = id;
        for (int guard = 0; guard < 256; ++guard) {
            const ecs::Transform* t = registry.get<ecs::Transform>(walk);
            if (t == nullptr || !t->parent.valid()) break;
            walk = t->parent;
            if (contains(selection, walk)) {
                nested = true;
                break;
            }
        }
        if (!nested) roots.push_back(id);
    }
    if (roots.empty()) return 0;
    m_entries.clear();
    std::vector<ecs::EntityID> order;
    for (ecs::EntityID root : roots) {
        const ecs::Transform* t = registry.get<ecs::Transform>(root);
        Entry e;
        e.components = captureEntityComponents(registry, root);
        e.originalParent = t != nullptr ? t->parent : ecs::EntityID::null();
        m_entries.push_back(std::move(e));
        order.push_back(root);
    }
    for (usize i = 0; i < order.size(); ++i) {
        const ecs::EntityID parent = order[i];
        auto& reg = const_cast<ecs::Registry&>(registry); // each<> is non-const; read only
        reg.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
            if (t.parent == parent && !contains(order, id)) {
                Entry e;
                e.components = captureEntityComponents(registry, id);
                e.parentEntry = static_cast<int>(i);
                m_entries.push_back(std::move(e));
                order.push_back(id);
            }
        });
    }
    return m_entries.size();
}

std::vector<ecs::EntityID> EntityClipboard::paste(ecs::Registry& registry, UndoStack& undo, EditorState& state) {
    std::vector<ecs::EntityID> roots;
    if (m_entries.empty()) return roots;
    std::vector<ecs::EntityID> created(m_entries.size(), ecs::EntityID::null());
    undo.beginMacro(m_entries.size() == 1u ? "Paste entity" : "Paste " + std::to_string(m_entries.size()) + " entities");
    for (usize i = 0; i < m_entries.size(); ++i) {
        EntityComponentSet c = m_entries[i].components;
        if (auto& t = std::get<std::optional<ecs::Transform>>(c); t) {
            if (m_entries[i].parentEntry >= 0) {
                t->parent = created[static_cast<usize>(m_entries[i].parentEntry)];
            } else {
                const ecs::EntityID p = m_entries[i].originalParent;
                t->parent = p.valid() && registry.alive(p) ? p : ecs::EntityID::null();
            }
            t->dirty = true;
        }
        auto command = std::make_unique<CreateEntityCommand>(registry, std::move(c), "Paste entity");
        CreateEntityCommand* raw = command.get();
        undo.execute(std::move(command));
        created[i] = raw->createdEntity();
        if (m_entries[i].parentEntry < 0 && created[i].valid()) roots.push_back(created[i]);
    }
    undo.endMacro();
    if (!roots.empty()) {
        state.selectedEntities = roots;
        state.primarySelection = roots.back();
        state.sceneModified = true;
    }
    return roots;
}

usize EntityClipboard::cut(ecs::Registry& registry, UndoStack& undo, EditorState& state) {
    std::vector<ecs::EntityID> selection = state.selectedEntities;
    if (selection.empty() && state.primarySelection.valid()) selection.push_back(state.primarySelection);
    const usize n = copy(registry, selection);
    if (n == 0) return 0;
    // Delete the copied subtrees, children first.
    std::vector<ecs::EntityID> doomed;
    for (ecs::EntityID id : selection) {
        if (registry.alive(id) && !contains(doomed, id)) doomed.push_back(id);
    }
    for (usize i = 0; i < doomed.size(); ++i) {
        const ecs::EntityID parent = doomed[i];
        registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
            if (t.parent == parent && !contains(doomed, id)) doomed.push_back(id);
        });
    }
    undo.beginMacro("Cut " + std::to_string(doomed.size()) + " entities");
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
        undo.execute(std::make_unique<DeleteEntityCommand>(registry, *it));
    }
    undo.endMacro();
    state.selectedEntities.clear();
    state.primarySelection = ecs::EntityID::null();
    state.sceneModified = true;
    return n;
}

} // namespace fuse::editor
