#include <fuse/editor/scene_hierarchy_panel.hpp>

namespace fuse::editor {

void SceneHierarchyPanel::setSceneRoot(Object* root) {
    m_model.setRoot(root);
}

void SceneHierarchyPanel::setSearchQuery(std::string query) {
    m_model.setSearchQuery(std::move(query));
}

void SceneHierarchyPanel::refresh() {
    m_model.rebuild();
}

void SceneHierarchyPanel::select(Handle<Object> object) {
    m_selection = object;
}

void SceneHierarchyPanel::reparentSelection(Object* newParent, UndoStack& undoStack) {
    if (!m_model.root() || !m_selection.isValid()) {
        return;
    }

    Object* selected = findObject_(m_model.root(), m_selection);
    if (!selected || selected == newParent) {
        return;
    }

    Object* oldParent = selected->parent();
    auto command = std::make_unique<ReparentObjectCommand>(*selected, newParent, oldParent);
    undoStack.execute(std::move(command));
    refresh();
}

u32 SceneHierarchyPanel::visibleNodeCount() const {
    return static_cast<u32>(m_model.visibleNodes().size());
}

Object* SceneHierarchyPanel::findObject_(Object* node, Handle<Object> handle) {
    if (!node) {
        return nullptr;
    }
    if (node->handle() == handle) {
        return node;
    }
    for (Object* child : node->children()) {
        Object* found = findObject_(child, handle);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

} // namespace fuse::editor
