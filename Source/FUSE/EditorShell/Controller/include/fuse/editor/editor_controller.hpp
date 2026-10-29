#pragma once
// Q1 seam (FUSE_EDITOR_QT_FIRST_PLAN.md §3.1): EditorController + ShellServices, the one Qt-free facade both editor
// shells call. It owns the scene mutex and the game-loop thread, runs the file / project flows (prompts through
// ShellServices), keeps the unified undo journal (Q2), runs the action table (Q6) and publishes what panels show as
// plain structs (EditorViewState, copied out under the lock; Q9 later swaps the lock for tick-published snapshots).
//
// First client: the FUSE UI shell (fuse_editor_fuse, FUSE_EDITOR_SHELL=fuse). The Qt shell (fuse_editor_qt) keeps its
// own MainWindow logic until it is routed through here (Q1 in the Qt tree).
#include <fuse/editor/component_schema.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_actions.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/runtime_viewport.hpp>
#include <fuse/editor/transform_gizmo.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/types.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fuse::editor {

class EditorHost;

enum class PromptAnswer : u8 { Save, Discard, Cancel };
struct Rgba {
    f32 r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

/// Dialogs and prompts the shell implements (Qt today, FUSE UI in fuse_editor_fuse, scripted in tests).
class ShellServices {
public:
    virtual ~ShellServices() = default;
    virtual std::optional<std::string> pickOpenFile(std::string_view title, std::string_view filter,
                                                    std::string_view directory) = 0;
    virtual std::optional<std::string> pickSaveFile(std::string_view title, std::string_view filter,
                                                    std::string_view directory) = 0;
    virtual std::optional<std::string> pickDirectory(std::string_view title, std::string_view directory) = 0;
    virtual std::optional<Rgba> pickColour(Rgba initial) = 0;
    virtual PromptAnswer askSaveDiscardCancel(std::string_view text) = 0;
    virtual void warn(std::string_view title, std::string_view text) = 0;
    /// Opens a file in the system editor (Qt QDesktopServices::openUrl). False when the shell cannot.
    virtual bool openExternal(std::string_view path) { (void)path; return false; }
};

/// An entity as the shells see it (no ECS types).
struct EntityRef {
    u32 index = 0;
    u32 generation = 0; ///< 0 = null (the ECS convention)
    [[nodiscard]] bool valid() const { return generation != 0; }
    friend bool operator==(const EntityRef&, const EntityRef&) = default;
};

struct HierarchyRowView {
    EntityRef entity;
    EntityRef parent;
    std::string label; ///< "SDF Object #3" (same text as the Qt hierarchy)
    u32 depth = 0;     ///< 0 at the top level (always 0 while filtering: matches are shown flat)
    bool selected = false;
};
struct InspectorFieldView {
    std::string label;
    std::string propertyName; ///< SetProperty key; empty for read-only rows
    PropertyFieldType type = PropertyFieldType::String;
    std::string value; ///< schema text format (see component_schema.hpp)
    std::vector<std::string> enumNames;
    f64 minValue = 0.0, maxValue = 0.0, step = 0.1;
};
struct InspectorSectionView {
    std::string componentName;
    std::string displayName;
    bool removable = false;
    bool typed = false; ///< schema kind (editable) vs read-only module section
    std::vector<InspectorFieldView> fields;
};
enum class TransportState : u8 { Stopped, Playing, Paused };

/// Everything the panels display, copied out of the host (never references into the scene).
struct EditorViewState {
    std::string title;      ///< "Untitled* - FUSE Editor" (same format as the Qt window title)
    std::string sceneName;
    std::string projectName;
    std::string scenePath;
    bool dirty = false;
    bool hasProject = false;
    TransportState transport = TransportState::Stopped;
    bool canUndo = false, canRedo = false;
    std::string undoLabel, redoLabel;
    std::vector<HierarchyRowView> hierarchy;
    u32 entityCount = 0;
    std::vector<EntityRef> selection;
    EntityRef primary;
    std::vector<InspectorSectionView> inspector;
    std::vector<std::string> addableComponents;
    std::string status; ///< status bar text
    u32 gameTicks = 0;
    std::vector<std::string> recentFiles;
    std::vector<std::string> recentProjects; ///< project roots, newest first (Qt File > Recent Projects)
    std::string message;                     ///< transient status message (Qt statusBar()->showMessage)
};

/// Normalised viewport input (no toolkit types).
enum class ViewportKey : u8 { W, A, S, D, Q, E, Shift };
struct ViewportPointerEvent {
    enum class Kind : u8 { Down, Move, Up } kind = Kind::Move;
    enum class Button : u8 { Left, Right, Middle } button = Button::Left;
    f32 x = 0.f, y = 0.f; ///< viewport pixels
};

/// Edit > Preferences general options (Qt EditorPreferences: prefs/previewSun, cameraSpeed, lookSensitivity,
/// restoreLayout). Qt-free; the FUSE shell persists them in the app settings file.
struct EditorPreferences {
    bool previewSun = true;
    f64 cameraSpeed = 10.0;
    f64 lookSensitivity = 0.2;
    bool restoreLayout = true;
};

class EditorController {
public:
    struct Options {
        std::string settingsPath; ///< JSON file for recent files (empty: not persisted, tests)
        u32 maxRecentFiles = 8;
    };
    EditorController(EditorHost& host, ShellServices& services);
    EditorController(EditorHost& host, ShellServices& services, Options options);
    ~EditorController();
    EditorController(const EditorController&) = delete;
    EditorController& operator=(const EditorController&) = delete;

    // ---- game thread ------------------------------------------------------------------------------------------
    void startGameLoop(f32 hz = 60.f);
    void stopGameLoop();
    [[nodiscard]] bool gameLoopRunning() const { return m_thread.joinable(); }
    /// Runs `frames` game ticks on the calling thread (tests / single-threaded hosts), then drains console output.
    void tick(u32 frames = 1);
    [[nodiscard]] f32 lastTickMs() const { return m_lastTickMs.load(std::memory_order_relaxed); }

    // ---- view state -------------------------------------------------------------------------------------------
    /// Rebuilds the view state from the host (takes the scene lock briefly). Returns true when it changed.
    bool refresh();
    [[nodiscard]] const EditorViewState& viewState() const { return m_view; }
    [[nodiscard]] u64 viewStateVersion() const { return m_version; }

    // ---- files and projects (flows; prompts through ShellServices) -------------------------------------------
    bool fileNew();
    bool fileOpen();
    bool fileOpenPath(const std::string& path); ///< without the picker (recent files, asset browser, tests)
    bool fileOpenRecent(u32 index);
    bool fileSave();
    bool fileSaveAs();
    bool requestClose(); ///< unsaved-changes prompt; true when the shell may close
    bool newProject(const std::string& directory, const std::string& name, u32 dimensionFlags);
    /// New Project wizard: also writes the module toggles to project.json and reopens it (Qt createProject).
    bool newProject(const std::string& directory, const std::string& name, u32 dimensionFlags,
                    const project::ModuleSettings& modules);
    bool openProject(const std::string& directory);
    /// File > Recent Projects (Qt rebuildRecentProjectsMenu): reopen by index; missing project.json fails.
    bool openRecentProject(u32 index);
    [[nodiscard]] static bool projectExists(const std::string& root);
    void clearRecentProjects();
    /// Status bar message (Qt showMessage); cleared by the next one.
    void showMessage(std::string text);
    [[nodiscard]] const std::string& lastError() const { return m_lastError; }
    [[nodiscard]] ShellServices& services() { return m_services; } ///< shell dialogs (pickers, warnings)

    // ---- edit -------------------------------------------------------------------------------------------------
    void undo(); ///< one history across UndoStack and CommandStack, in edit order
    void redo();

    // ---- selection, hierarchy, inspector ----------------------------------------------------------------------
    void select(std::span<const EntityRef> entities, EntityRef primary);
    void selectOnly(EntityRef entity);
    /// Posts one reparent command per entity (cycles and no-ops skipped). Returns the number posted.
    u32 reparent(std::span<const EntityRef> entities, EntityRef newParent);
    void setHierarchyFilter(std::string_view text);
    [[nodiscard]] const std::string& hierarchyFilter() const { return m_filter; }
    void setProperty(EntityRef entity, std::string_view propertyName, std::string_view value);
    void addComponent(EntityRef entity, std::string_view componentName);
    void removeComponent(EntityRef entity, std::string_view componentName);
    /// Entity menu / context-menu action on the current selection (create spawns at the origin). True on change.
    bool runEntityAction(ContextMenuAction action);
    [[nodiscard]] EntityRef lastCreated() const { return m_lastCreated; }
    /// Entity > Duplicate (Qt): whole subtrees with their display names, one undo step; the copies are selected.
    bool duplicateSelection();
    /// Entity > Group Selection / Ungroup (Qt, shared groupSelection / ungroupSelection; one undo step each).
    bool groupSelection();
    bool ungroupSelection();
    /// Absolute path of the primary selection's Lua script (relative paths resolve against the script root);
    /// empty when it has no Script or the file is missing (Qt selectedScriptPath).
    [[nodiscard]] std::string selectedScriptPath();
    /// Entity > Open Script: ShellServices::openExternal on selectedScriptPath(). False when there is none.
    bool openScript();

    // ---- console ----------------------------------------------------------------------------------------------
    [[nodiscard]] ConsolePanel& console() { return m_console; }
    void submitConsole(std::string_view line);
    [[nodiscard]] u64 consoleVersion() const { return m_consoleVersion; }

    // ---- viewport (UI-thread camera, like the Qt placeholder) ------------------------------------------------
    [[nodiscard]] ViewportPanel& viewport() { return m_viewport; }
    void viewportResize(u32 width, u32 height);
    void viewportKey(ViewportKey key, bool down);
    void viewportPointer(const ViewportPointerEvent& event);
    void viewportWheel(f32 notches);
    /// Advances the fly camera by dt (call from the shell's frame tick while flying / keys held).
    bool viewportTick(f32 dt);
    [[nodiscard]] bool viewportFlying() const { return m_rmbHeld && m_rmbDragged; }
    /// A right click (no drag) in the viewport opened the entity context menu: the shell shows `contextMenu()`
    /// at contextMenuAt() and calls activateContextMenu with the chosen item (or closeContextMenu).
    [[nodiscard]] bool contextMenuOpen() const { return m_entityMenu.isOpen(); }
    [[nodiscard]] const EntityContextMenu& contextMenu() const { return m_entityMenu; }
    void openContextMenu(f32 viewportX, f32 viewportY);
    bool activateContextMenu(ContextMenuAction action);
    void closeContextMenu() { m_entityMenu.close(); }
    struct ViewportMarker {
        EntityRef entity;
        f32 x = 0.f, y = 0.f; ///< viewport pixels
        f32 depth = 0.f;
        u8 kind = 0;           ///< 0 entity, 1 mesh, 2 sdf, 3 light, 4 camera
        bool selected = false;
    };
    /// Entities projected through the viewport camera (for the shell's software viewport).
    std::vector<ViewportMarker> viewportMarkers();
    // ---- transform gizmo (Qt parity: viewport_placeholder_widget) -------------------------------------------
    // Left press on a handle of the primary selection's gizmo starts a drag (one undo macro per drag, multi-select
    // follows); a left click elsewhere picks. Tools: Select (no gizmo), Translate, Rotate, Scale (Qt keys 1-4).
    void setTransformTool(TransformTool tool);
    [[nodiscard]] TransformTool transformTool() const { return m_gizmo.settings().tool; }
    void setTransformSpace(TransformSpace space) { m_gizmo.settings().space = space; ++m_gizmoVersion; }
    void setTransformSnap(bool on) { m_gizmo.settings().snap = on; ++m_gizmoVersion; }
    [[nodiscard]] TransformSpace transformSpace() const { return m_gizmo.settings().space; }
    [[nodiscard]] bool transformSnap() const { return m_gizmo.settings().snap; }
    struct GizmoView {
        bool visible = false;
        TransformTool tool = TransformTool::Translate;
        std::array<GizmoHandle2D, 3> handles{}; ///< viewport pixels, axis 0 X / 1 Y / 2 Z
        int dragAxis = -1;                      ///< axis being dragged
    };
    /// Lays the gizmo out for the primary selection through the current camera (draw it over the viewport).
    GizmoView viewportGizmo();
    // ---- view modes (Qt View > View Mode, Show Mesh Bounds) -------------------------------------------------
    // The mode is the editor-wide setting the Qt viewport reads (setEditorViewMode): Unlit / Wireframe render
    // surfaces unlit, Wireframe / Lit Wireframe overlay the real triangle edges of every mesh and SDF proxy.
    /// Applies Edit > Preferences: preview sun (shared editor setting), fly speed and look sensitivity.
    void applyPreferences(const EditorPreferences& p);
    [[nodiscard]] EditorPreferences preferences() const { return m_prefs; }
    void setViewMode(EditorViewMode mode);
    [[nodiscard]] EditorViewMode viewMode() const { return editorViewMode(); }
    void setShowMeshBounds(bool on);
    [[nodiscard]] bool showMeshBounds() const { return m_showMeshBounds; }
    struct ViewportSegment {
        f32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; ///< viewport pixels
        u8 role = 0; ///< 0 mesh bounds, 1 selected bounds, 3 wire edge, 4 selected wire edge (Qt colour roles)
    };
    /// Triangle edges of an engine mesh id (x0 y0 z0 x1 y1 z1 per edge, object space); null when unknown.
    using MeshEdgesFn = const std::vector<f32>* (*)(void* user, u32 engineMeshId);
    /// Projected wireframe (wire view modes) and mesh-bounds (Show Mesh Bounds) segments, at most `budget` edges.
    std::vector<ViewportSegment> viewportSegments(MeshEdgesFn edges, void* user, usize budget = 400000);
    /// View > Show All Colliders: collider wireframes of every entity (selected entities always show theirs).
    void setShowAllColliders(bool on);
    [[nodiscard]] bool showAllColliders() const { return m_showAllColliders; }
    /// Play > Maximize Viewport on Play (Qt): the shell hides the docks while playing; Stop restores them.
    void setMaximizeOnPlay(bool on) { m_maximizeOnPlay = on; }
    [[nodiscard]] bool maximizeOnPlay() const { return m_maximizeOnPlay; }
    /// View > Viewport Layout (Qt GUI-8): 0 single, 1 two (perspective + Top), 2 quad (+ Front, Side).
    void setViewportLayout(int layout) { m_viewportLayout = layout < 0 ? 0 : layout > 2 ? 2 : layout; }
    [[nodiscard]] int viewportLayout() const { return m_viewportLayout; }
    /// View > Frame Selected (Qt frameSelection): moves the camera back along its forward to fit the selection.
    bool frameSelection();
    /// Collider drag handles of the primary selection (Qt viewport): Box faces (half extents), Sphere radius,
    /// Capsule radius / half height. A left press within 8 px drags it (one undo step per drag).
    struct ColliderHandleView {
        f32 x = 0.f, y = 0.f;         ///< viewport pixels
        f32 axisX = 0.f, axisY = 0.f; ///< pixels per local unit outward
        int param = 0;                ///< Collider.params component
        f32 sign = 1.f;
    };
    [[nodiscard]] std::vector<ColliderHandleView> colliderHandles();
    [[nodiscard]] bool colliderDragging() const { return m_colliderDrag >= 0; }
    [[nodiscard]] std::size_t colliderDragHandle() const { return std::size_t(m_colliderDrag); }
    [[nodiscard]] bool gizmoDragging() const { return m_gizmo.dragging(); }
    /// Bumped by tool / drag changes so shells know to redraw the overlay.
    [[nodiscard]] u64 gizmoVersion() const { return m_gizmoVersion; }
    /// Projects a world point through the viewport camera; false when it is behind the camera.
    [[nodiscard]] bool projectPoint(f32 x, f32 y, f32 z, f32& px, f32& py) const;

    // ---- actions ----------------------------------------------------------------------------------------------
    [[nodiscard]] bool actionEnabled(ActionId id) const;
    bool trigger(ActionId id);
    /// Set by the shell for actions it handles itself (Quit, Reset Layout, New Project wizard).
    using ShellActionHook = bool (*)(void* user, ActionId id);
    void setShellActionHook(ShellActionHook hook, void* user) { m_shellHook = hook; m_shellUser = user; }

    [[nodiscard]] EditorHost& hostForTests() { return m_host; }
    [[nodiscard]] std::mutex& sceneMutexForTests() { return m_mutex; }

private:
    friend class EditorPanels; // panel models share the scene mutex, host, viewport camera and picker
    void syncUndoJournal_();
    bool maybeSave_();
    void addRecent_(const std::string& path);
    void loadSettings_();
    void saveSettings_() const;
    void drainConsole_();
    std::string sceneDisplayName_() const;

    EditorHost& m_host;
    ShellServices& m_services;
    Options m_options;
    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<f32> m_lastTickMs{0.f};

    EditorViewState m_view;
    u64 m_version = 0;
    std::string m_viewKey;
    std::string m_filter;
    std::string m_lastError;
    std::vector<std::string> m_recent;

    enum class UndoSource : u8 { Scene, Property };
    std::vector<UndoSource> m_undoJournal, m_redoJournal;
    u32 m_seenSceneUndo = 0, m_seenPropertyUndo = 0;

    PropertyInspector m_inspector;
    EntityContextMenu m_entityMenu;
    EntityRef m_lastCreated;
    ConsolePanel m_console;
    u64 m_consoleVersion = 0;

    ViewportPanel m_viewport;
    ViewportInput m_input{};
    std::unique_ptr<class ViewportSceneView> m_sceneView;
    bool m_rmbHeld = false, m_rmbDragged = false;
    f32 m_downX = 0.f, m_downY = 0.f;
    f32 m_lastX = 0.f, m_lastY = 0.f;
    bool m_keys[7]{};

    struct GizmoGroupStart {
        ecs::EntityID id;
        TransformCommand::State state;
        bool sameFrame = false;
    };
    TransformGizmo m_gizmo;
    ecs::EntityID m_gizmoEntity = ecs::EntityID::null();
    std::vector<GizmoGroupStart> m_gizmoGroup;
    bool m_gizmoMacroOpen = false;
    u64 m_gizmoVersion = 0;
    bool m_showMeshBounds = false;
    bool m_showAllColliders = false;
    bool m_maximizeOnPlay = false;
    int m_viewportLayout = 0;
    std::vector<std::string> m_recentProjects;
    std::string m_message;
    std::vector<ColliderHandleView> m_colliderHandleCache; ///< laid out at drag start
    ecs::EntityID m_colliderEntity{};
    int m_colliderDrag = -1;
    f32 m_colliderStartX = 0.f, m_colliderStartY = 0.f;
    std::shared_ptr<void> m_colliderBefore; ///< ecs::Collider at drag start
    std::vector<ColliderHandleView> colliderHandles_(ecs::EntityID& entity); // m_mutex held
    void addRecentProject_(const std::string& root);
    bool colliderPointer_(const ViewportPointerEvent& e);
    EditorPreferences m_prefs;
    bool refreshGizmo_(); // m_mutex held
    bool gizmoPress_(f32 px, f32 py);
    void gizmoMove_(f32 px, f32 py);
    void gizmoRelease_();

    ShellActionHook m_shellHook = nullptr;
    void* m_shellUser = nullptr;
};

} // namespace fuse::editor
