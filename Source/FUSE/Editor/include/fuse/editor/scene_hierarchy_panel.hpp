#pragma once

#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/hierarchy_model.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

/// Headless scene hierarchy panel API (B6.5).
class SceneHierarchyPanel {
public:
    void setSceneRoot(Object* root);
    void setSearchQuery(std::string query);
    void refresh();

    void select(Handle<Object> object);
    Handle<Object> selection() const { return m_selection; }

    const HierarchyModel& model() const { return m_model; }
    HierarchyModel& model() { return m_model; }

    void reparentSelection(Object* newParent, UndoStack& undoStack);

    u32 visibleNodeCount() const;

private:
    static Object* findObject_(Object* node, Handle<Object> handle);

    HierarchyModel m_model;
    Handle<Object> m_selection = Handle<Object>::invalid();
};

} // namespace fuse::editor
