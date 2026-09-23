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

/// Where the viewport sits inside the top-level editor window, as the Qt shell reports it.
/// Window coordinates are logical (device-independent) pixels, origin top-left, +y down (Qt
/// convention). The viewport's own pixel space (`ViewportPanel::screenRay`, `openInViewport`
/// px/py) is physical framebuffer pixels = logical * devicePixelRatio.
struct ContextMenuHostGeometry {
    f32 viewportOriginX = 0.f; ///< viewport top-left in window logical px
    f32 viewportOriginY = 0.f;
    f32 devicePixelRatio = 1.f; ///< physical px per logical px (Qt `devicePixelRatioF`); <= 0 treated as 1
    f32 windowWidth = 0.f;      ///< window client size in logical px (0 = unbounded, no clamping)
    f32 windowHeight = 0.f;
};

/// Screen placement of the open menu, in window logical px (what `QMenu::popup` takes).
struct ContextMenuPlacement {
    f32 clickX = 0.f; ///< the click, mapped viewport physical px -> window logical px
    f32 clickY = 0.f;
    f32 x = 0.f; ///< menu top-left
    f32 y = 0.f;
    f32 width = 0.f;
    f32 height = 0.f;
    bool flippedX = false; ///< opened to the left of the click (would overflow the right edge)
    bool flippedY = false; ///< opened above the click (would overflow the bottom edge)
};

/// Right-click context menu model for the viewport and hierarchy (B6.3 / B6.5). Opening it over
/// an entity selects that entity; create actions spawn at the clicked surface (or in front of the
/// camera on empty space). Every action runs through the `UndoStack` as one undoable step.
class EntityContextMenu {
public:
    static constexpr f32 kSpawnDistance = 5.f;
    /// Menu metrics in logical px (Fusion-style QMenu defaults).
    static constexpr f32 kMenuWidth = 200.f;
    static constexpr f32 kItemHeight = 22.f;
    static constexpr f32 kSeparatorHeight = 7.f;
    static constexpr f32 kMenuPadding = 4.f; ///< top and bottom frame each

    /// Viewport placement inside the window used by `openInViewport` to position the menu.
    void setHostGeometry(const ContextMenuHostGeometry& geometry) { m_host = geometry; }
    [[nodiscard]] const ContextMenuHostGeometry& hostGeometry() const { return m_host; }

    /// Map viewport physical pixel (px, py) to window logical px.
    static void viewportToWindow(const ContextMenuHostGeometry& host, f32 px, f32 py, f32& wx,
                                               f32& wy);
    /// QMenu::popup placement: top-left at the click; flipped to the left / above when it would
    /// overflow the right / bottom window edge; then clamped inside the window (menus larger than
    /// the window pin to the top-left).
    [[nodiscard]] static ContextMenuPlacement place(const ContextMenuHostGeometry& host, f32 windowX, f32 windowY,
                                                    f32 menuWidth, f32 menuHeight);
    /// Logical size of the current item list.
    [[nodiscard]] f32 menuWidth() const { return kMenuWidth; }
    [[nodiscard]] f32 menuHeight() const;

    /// Viewport right-click at pixel (px, py): picks through `view`, updates selection, and
    /// records the spawn point for create actions.
    void openInViewport(EditorScene& scene, ViewportSceneView& view, const ViewportPanel& viewport,
                        f32 px, f32 py, EditorState& state);
    /// Hierarchy right-click on `clicked` (null = empty space); spawns at `spawnPoint`.
    void openAt(ecs::EntityID clicked, const ecs::vec3& spawnPoint, EditorState& state);
    /// Hierarchy right-click with the click position in window logical px (positions the menu).
    void openAt(ecs::EntityID clicked, const ecs::vec3& spawnPoint, EditorState& state, f32 windowX, f32 windowY);
    /// Placement computed by the last open (viewport opens map px/py through `hostGeometry()`).
    [[nodiscard]] const ContextMenuPlacement& placement() const { return m_placement; }
    /// Re-place the open menu for the size the host actually shows (e.g. `QMenu::sizeHint`),
    /// keeping the recorded click: flip / clamp decisions then use the real menu size.
    void setMenuSize(f32 width, f32 height);
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
    ContextMenuHostGeometry m_host{};
    ContextMenuPlacement m_placement{};
    bool m_open = false;
};

} // namespace fuse::editor
