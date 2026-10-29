#include <fuse/editor/entity_clipboard.hpp>

#include <algorithm>

namespace fuse::editor {

namespace {
bool contains(const std::vector<ecs::EntityID>& v, ecs::EntityID id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}
} // namespace

usize EntityClipboard::copy(const ecs::Registry& registry, const std::vector<ecs::EntityID>& selection,
                           const EntityNames* names) {
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
        if (names != nullptr) e.name = names->get(root);
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
                if (names != nullptr) e.name = names->get(id);
                m_entries.push_back(std::move(e));
                order.push_back(id);
            }
        });
    }
    return m_entries.size();
}

std::vector<ecs::EntityID> EntityClipboard::paste(ecs::Registry& registry, UndoStack& undo, EditorState& state,
                                                  EntityNames* names) {
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
        if (names != nullptr && created[i].valid() && !m_entries[i].name.empty()) {
            undo.execute(std::make_unique<RenameEntityCommand>(*names, created[i], m_entries[i].name));
        }
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

usize EntityClipboard::cut(ecs::Registry& registry, UndoStack& undo, EditorState& state, EntityNames* names) {
    std::vector<ecs::EntityID> selection = state.selectedEntities;
    if (selection.empty() && state.primarySelection.valid()) selection.push_back(state.primarySelection);
    const usize n = copy(registry, selection, names);
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

ecs::EntityID groupSelection(ecs::Registry& registry, UndoStack& undo, EditorState& state, EntityNames* names,
                             const std::string& name) {
    std::vector<ecs::EntityID> selection = state.selectedEntities;
    if (selection.empty() && state.primarySelection.valid()) selection.push_back(state.primarySelection);
    std::vector<ecs::EntityID> roots;
    for (ecs::EntityID id : selection) {
        const ecs::Transform* t = registry.alive(id) ? registry.get<ecs::Transform>(id) : nullptr;
        if (t == nullptr || contains(roots, id)) continue;
        bool nested = false;
        ecs::EntityID walk = id;
        for (int guard = 0; guard < 256; ++guard) {
            const ecs::Transform* wt = registry.get<ecs::Transform>(walk);
            if (wt == nullptr || !wt->parent.valid()) break;
            walk = wt->parent;
            if (contains(selection, walk)) {
                nested = true;
                break;
            }
        }
        if (!nested) roots.push_back(id);
    }
    if (roots.empty()) return ecs::EntityID::null();
    const ecs::EntityID parent = registry.get<ecs::Transform>(roots.front())->parent;
    f32 c[3] = {0.f, 0.f, 0.f};
    for (ecs::EntityID id : roots) {
        const ecs::Transform* t = registry.get<ecs::Transform>(id);
        if (t->parent != parent) return ecs::EntityID::null();
        c[0] += t->position.x;
        c[1] += t->position.y;
        c[2] += t->position.z;
    }
    const f32 inv = 1.f / static_cast<f32>(roots.size());
    EntityComponentSet set{};
    ecs::Transform gt{};
    gt.position = {c[0] * inv, c[1] * inv, c[2] * inv, 1.f};
    gt.parent = parent;
    gt.dirty = true;
    std::get<std::optional<ecs::Transform>>(set) = gt;
    undo.beginMacro("Group " + std::to_string(roots.size()) + " entities");
    auto create = std::make_unique<CreateEntityCommand>(registry, std::move(set), "Create group");
    CreateEntityCommand* raw = create.get();
    undo.execute(std::move(create));
    const ecs::EntityID group = raw->createdEntity();
    if (group.valid()) {
        if (names != nullptr && !name.empty()) undo.execute(std::make_unique<RenameEntityCommand>(*names, group, name));
        for (ecs::EntityID id : roots) {
            // Same parent frame, identity group rotation / scale: the local offset is position - centroid.
            const ecs::Transform* t = registry.get<ecs::Transform>(id);
            const TransformCommand::State before = TransformCommand::capture(*t);
            TransformCommand::State after = before;
            after.position.x -= gt.position.x;
            after.position.y -= gt.position.y;
            after.position.z -= gt.position.z;
            undo.execute(std::make_unique<ReparentEntityCommand>(registry, id, group, parent));
            undo.execute(std::make_unique<TransformCommand>(registry, id, before, after));
        }
    }
    undo.endMacro();
    if (group.valid()) {
        state.selectedEntities = {group};
        state.primarySelection = group;
        state.sceneModified = true;
    }
    return group;
}

} // namespace fuse::editor
