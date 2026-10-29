#pragma once
// Editor copy / cut / paste of entity subtrees (Qt-free; UI or game thread under the scene lock).

#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/entity_names.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

class EntityClipboard {
public:
    /// Copy the selection and all Transform descendants. Returns entities copied.
    usize copy(const ecs::Registry& registry, const std::vector<ecs::EntityID>& selection,
               const EntityNames* names = nullptr);
    /// Paste as one undo step; copied roots keep their original parent while it is alive.
    /// Selects the pasted roots in `state`. Returns the new roots.
    std::vector<ecs::EntityID> paste(ecs::Registry& registry, UndoStack& undo, EditorState& state,
                                     EntityNames* names = nullptr);
    /// Cut = copy + delete the selection (one undo step).
    usize cut(ecs::Registry& registry, UndoStack& undo, EditorState& state, EntityNames* names = nullptr);
    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] usize size() const { return m_entries.size(); }

private:
    struct Entry {
        EntityComponentSet components;
        int parentEntry = -1;                               ///< index in m_entries, or -1 (root)
        ecs::EntityID originalParent = ecs::EntityID::null(); ///< for roots
        std::string name;                                      ///< editor display name
    };
    std::vector<Entry> m_entries; ///< parent-first
};

/// Group: one undo step that creates an empty Transform parent at the centroid of the selected roots
/// (all must share one parent, which the group gets) and moves them under it, keeping their world
/// pose. The group is named `name` (when `names` is given) and becomes the selection. Null when the
/// selection is empty or the roots have different parents.
ecs::EntityID groupSelection(ecs::Registry& registry, UndoStack& undo, EditorState& state, EntityNames* names = nullptr,
                             const std::string& name = "Group");

} // namespace fuse::editor
