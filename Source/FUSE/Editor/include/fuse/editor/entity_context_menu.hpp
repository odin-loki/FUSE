#pragma once

#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_scene_view.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::editor {

enum class ContextMenuAction : u8 {
    CreateEmpty,
    CreateCube,          ///< Mesh placeholder with unit bounds (asset bound later in the inspector)
    CreateSdfSphere,
    CreateSdfBox,
    CreatePointLight,
    CreateDirectionalLight,
    CreateCamera,
    Duplicate,
    Delete,
};

struct ContextMenuItem {
    ContextMenuAction action = ContextMenuAction::CreateEmpty;
    const char* label = "";
    bool enabled = true;
    bool separatorBefore = false;
};

/// Right-click context menu model for the viewport and hierarchy (B6.3 / B6.5). Opening it over
/// an entity selects that entity; create actions spawn at the clicked surface (or in front of the
/// camera on empty space). Every action runs through the `UndoStack` as one undoable step.
class EntityContextMenu {
public:
    static constexpr f32 kSpawnDistance = 5.f;

    /// Viewport right-click at pixel (px, py): picks through `view`, updates selection, and
    /// records the spawn point for create actions.
    void openInViewport(EditorScene& scene, ViewportSceneView& view, const ViewportPanel& viewport,
                        f32 px, f32 py, EditorState& state);
    /// Hierarchy right-click on `clicked` (null = empty space); spawns at `spawnPoint`.
    void openAt(ecs::EntityID clicked, const ecs::vec3& spawnPoint, EditorState& state);
    void close() { m_open = false; }

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const std::vector<ContextMenuItem>& items() const { return m_items; }
    [[nodiscard]] const ContextMenuItem* find(ContextMenuAction action) const;
    [[nodiscard]] ecs::EntityID clickedEntity() const { return m_clicked; }
    [[nodiscard]] const ecs::vec3& spawnPoint() const { return m_spawnPoint; }

    /// Run `action` (must be an enabled item of the open menu). Returns false when disabled or
    /// nothing changed. Creates select the new entity; Delete removes the whole selection.
    bool activate(ContextMenuAction action, EditorScene& scene, EditorState& state, UndoStack& undo);

    /// Entity created by the last create / duplicate action.
    [[nodiscard]] ecs::EntityID lastCreated() const { return m_lastCreated; }

    /// Component set a create action spawns at `position` (exposed for panels and tests).
    [[nodiscard]] static EntityComponentSet componentsFor(ContextMenuAction action, const ecs::vec3& position);

private:
    void rebuildItems_(const EditorState& state);

    std::vector<ContextMenuItem> m_items;
    ecs::EntityID m_clicked = ecs::EntityID::null();
    ecs::vec3 m_spawnPoint{};
    ecs::EntityID m_lastCreated = ecs::EntityID::null();
    bool m_open = false;
};

} // namespace fuse::editor
