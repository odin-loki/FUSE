#pragma once
// UI9 (FUSE_EDITOR_QT_FIRST_PLAN.md): the editor chrome built with FUSE UI (FUSE_EDITOR_SHELL=fuse), beside the Qt
// shell and driven by the same Qt-free EditorController. Same panel ids (dock.hierarchy, dock.inspector, ...), the
// same action table (menus, toolbar, shortcuts) and the same window title, so the fuse_editor_ui_* gates mirror the
// fuse_editor_qt_* ones.
//
// Layout: menu bar / toolbar (main + transport) / dock area (left: Hierarchy | Project Hub | Asset Browser,
// right: Inspector | Material Editor | SDF Sculpt, bottom: Console | Profiler | Sequencer | Timeline; FUSE UI docks
// stack, so each side is one dock holding a tab widget) around a software-rendered viewport / status bar.
#include <fuse/editor/editor_controller.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/editor_panel_models.hpp>
#include <fuse/editor/scene_viewport_host.hpp>

#include "fuse/app/app.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::editor::fui {

using fuse::ui::NodeHandle;

struct EditorShellOptions {
    f32 fontScale = 1.f; ///< 1, 1.25, 1.5, 2 (the Qt font_* gates)
};

/// One panel of the Qt shell's dock set: its Qt objectName-style id, tab title and side.
struct ShellPanel {
    const char* id;
    const char* title;
    fuse::ui::widgets::DockSide side;
    bool ported; ///< false: placeholder page (panel not ported to FUSE UI yet)
};
[[nodiscard]] std::span<const ShellPanel> shellPanels();

/// Theme CSS from the Q8 tokens (kEditorDarkTheme) plus the shell's own rules.
[[nodiscard]] std::string editorShellCss(f32 fontScale);

class EditorShellWindow {
public:
    EditorShellWindow(fuse::app::AppWindow& window, EditorController& controller, EditorShellOptions options = {});
    ~EditorShellWindow();
    EditorShellWindow(const EditorShellWindow&) = delete;
    EditorShellWindow& operator=(const EditorShellWindow&) = delete;

    /// Pulls the controller state into the widgets (called by a UI timer; idempotent when nothing changed).
    /// Returns true when something was updated.
    bool sync();
    /// One shell frame (the 30 Hz UI timer): sync() plus per-frame work (profiler sample, timeline playback).
    void frame();
    /// Restores the default dock layout and tab pages (View > Reset Layout).
    void resetLayout();
    /// Makes `panelId`'s tab current (e.g. "dock.console").
    bool showPanel(std::string_view panelId);
    /// File > New Project...: the New Project wizard dialog (name, location, dimension, UI layer, modules; Qt
    /// NewProjectWizard parity). Ids "wizard.*"; Create writes project.json and opens the project.
    void openNewProjectWizard();
    /// Where the last viewport context menu was placed (Qt placement rules; context_menu gates).
    [[nodiscard]] const ContextMenuPlacement& contextMenuPlacement() const { return m_menuPlacement; }
    [[nodiscard]] bool newProjectWizardOpen() const { return m_wizard.valid(); }
    /// Edit > Preferences... (Qt PreferencesDialog parity): General options (preview sun, camera speed, look
    /// sensitivity, restore layout) and the Shortcuts list with a filter and the conflict check. Ids "prefs.*".
    /// OK / Apply apply them (EditorController::applyPreferences) and persist them in the app settings
    /// ("prefs.previewSun", ...); they are loaded at startup.
    void openPreferences();
    [[nodiscard]] bool preferencesOpen() const { return m_prefsDialog.valid(); }
    /// "key: action, action" for every shortcut bound to more than one action (Qt PreferencesDialog::conflicts).
    [[nodiscard]] static std::vector<std::string> shortcutConflicts();
    /// Same check over a binding table (one canonical combo per ActionId, "" = unbound).
    [[nodiscard]] static std::vector<std::string> shortcutConflicts(std::span<const std::string> bindings);
    /// Canonical "Ctrl+Alt+Shift+K" form of a combo string ("" when it does not parse).
    [[nodiscard]] static std::string normalizeShortcut(std::string_view text);
    [[nodiscard]] static std::string shortcutText(fuse::ui::Key key, fuse::ui::u8 mods);
    /// Effective shortcut of an action (default from the action table unless rebound in Preferences).
    [[nodiscard]] const std::string& binding(ActionId id) const { return m_bindings[std::size_t(id)]; }
    /// Preferences dialog: rebinds `stableId` ("entity.group" or the Qt objectName) in the pending table and
    /// refreshes the conflict line (Qt PreferencesDialog::setShortcutFor). Applied by OK / Apply.
    bool setPreferencesShortcut(std::string_view stableId, std::string_view combo);
    /// Preferences dialog: pending conflicts (what OK / Apply would refuse on).
    [[nodiscard]] std::vector<std::string> preferencesConflicts() const { return shortcutConflicts(m_prefsBindings); }

    [[nodiscard]] fuse::app::AppWindow& window() { return m_win; }
    [[nodiscard]] EditorController& controller() { return m_c; }
    [[nodiscard]] NodeHandle node(std::string_view id) const;
    [[nodiscard]] NodeHandle panelPage(std::string_view panelId) const;
    /// True when `panelId` ("profiler" or "dock.profiler") is the current tab of its dock.
    [[nodiscard]] bool panelVisible(std::string_view panelId) const;
    /// The Qt-free models behind the seven UI9 panels.
    [[nodiscard]] EditorPanels& panels() { return *m_models; }
    [[nodiscard]] NodeHandle viewportNode() const { return m_viewport; }
    [[nodiscard]] const fuse::ui::Image& viewportImage() const { return m_image; }
    [[nodiscard]] u32 viewportRenders() const { return m_renders; }

    /// Live 3D viewport: the scene renderer host draws the edited scene each time the view changes (camera, size,
    /// scene / material edits; every frame while playing). With `present` set (a GPU-drawn window on the host's
    /// device) the viewport image becomes fuse_ui_vk external slot 0 and `present` is handed every new frame to bind
    /// it (NativePlatform::setExternalTexture): zero-copy. Without it the frame is read back into the viewport image
    /// (CPU raster / headless / screenshots). Null host: the software grid + markers viewport.
    void setSceneViewport(SceneViewportHost* host, std::function<void(const SceneViewportFrame&)> present = {});
    [[nodiscard]] SceneViewportHost* sceneViewport() const { return m_scene; }
    [[nodiscard]] u32 sceneFrames() const { return m_sceneFrames; }
    [[nodiscard]] u32 syncCount() const { return m_syncs; }
    /// Play-mode frame (Qt playBannerState): 0 none, 1 PLAYING (green), 2 PAUSED (amber); badge text, session clock
    /// (runs only while unpaused) and game ticks since Play.
    [[nodiscard]] int playBannerState() const { return m_playState; }
    [[nodiscard]] std::string playBadge() const;
    [[nodiscard]] f64 playElapsedMs() const;
    [[nodiscard]] u32 playTicks() const { return m_playTicks; }
    /// True while Maximize Viewport on Play has the docks hidden.
    [[nodiscard]] bool viewportMaximized() const { return m_maximized; }
    /// Multi-viewport (Qt GUI-8): orthographic Top / Front / Side views (0..2) beside the perspective viewport,
    /// shown by View > Viewport Layout (Ctrl+Alt+1..3). Node ids "viewport.ortho.top|front|side".
    [[nodiscard]] NodeHandle orthoNode(int i) const { return m_ortho[i]; }
    [[nodiscard]] OrthoView& orthoView(int i) { return m_orthoView[i]; }
    [[nodiscard]] u32 orthoDrawn(int i) const { return m_orthoDrawn[i]; }
    /// Menu line -> action / recent index mapping, for gates that drive the menus by item index.
    [[nodiscard]] i32 menuItemIndex(std::string_view menu, ActionId id) const;
    [[nodiscard]] NodeHandle menuNode(std::string_view menu) const;

    /// Shortcut handling shared by the root capture handler and tests. True when consumed.
    bool shortcut(fuse::ui::Key key, u8 mods);
    /// Root capture hook (viewport keys / pointer bookkeeping). Internal.
    bool rootEvent(const fuse::ui::Event& e, NodeHandle target);

private:
    void build_();
    void buildMenus_();
    void buildToolbar_(NodeHandle parent);
    void buildHierarchy_(NodeHandle page);
    void buildInspector_(NodeHandle page);
    void buildConsole_(NodeHandle page);
    void buildPlaceholder_(NodeHandle page, const ShellPanel& p);
    void syncMenus_();
    void syncToolbar_();
    void syncHierarchy_();
    void syncInspector_();
    void syncConsole_();
    void renderViewport_();
    void menuPicked_(u32 menuIndex, u32 line);
    void inspectorEdited_(u32 row);
    void viewportInput_();

    fuse::app::AppWindow& m_win;
    fuse::ui::UiContext& m_ui;
    EditorController& m_c;
    EditorShellOptions m_opt;

    NodeHandle m_root, m_menuBar, m_toolbar, m_dockArea, m_viewport, m_viewportInfo, m_status;
    NodeHandle m_docks[3]{};
    NodeHandle m_tabs[3]{};
    std::vector<NodeHandle> m_menus;
    std::vector<std::vector<i32>> m_menuMap; ///< per menu, per line: ActionId, -1 separator, -(2 + i) recent file i
    std::vector<std::pair<ActionId, NodeHandle>> m_toolButtons;

    NodeHandle m_hierFilter, m_hierTree, m_hierInfo;
    std::vector<EntityRef> m_hierRows; ///< tree line -> entity
    std::string m_hierText;

    NodeHandle m_inspTitle, m_inspGrid, m_inspAddCombo, m_inspRemoveCombo, m_inspBox;
    struct GridRow { std::string property; PropertyFieldType type; std::vector<std::string> enums; };
    std::vector<GridRow> m_gridRows;
    std::string m_inspKey;
    EntityRef m_inspEntity;

    NodeHandle m_consoleList, m_consoleInput;
    u64 m_consoleSeen = ~0ull;

    fuse::ui::Image m_image;
    u32 m_imageIndex = 0;
    u32 m_renders = 0, m_syncs = 0;
    SceneViewportHost* m_scene = nullptr;
    std::function<void(const SceneViewportFrame&)> m_scenePresent;
    u32 m_sceneFrames = 0;
    u64 m_sceneVersion = ~0ull;
    bool renderSceneViewport_(int wpx, int hpx, bool changed);
    void updateOverlay_(); // transform gizmo over the viewport (CPU or zero-copy image)
    NodeHandle m_gizmoTools[4]{};
    NodeHandle m_gizmoSpace{}, m_gizmoSnap{};
    NodeHandle m_wizard{};
    NodeHandle m_prefsDialog{};
    std::vector<std::string> m_bindings, m_prefsBindings; ///< per ActionId (effective / pending in Preferences)
    std::vector<ActionId> m_prefsRows;                    ///< Preferences list row -> action
    std::string prefsRows_(std::string_view filter);
    void prefsRefresh_();
    void loadBindings_();
    void closePreferences_();
    bool applyPreferencesDialog_();
    ContextMenuPlacement m_menuPlacement{};
    void closeNewProjectWizard_();
    bool m_spaceShown = false;
    std::string m_overlayKey;
    u32 m_toolShown = ~0u;
    u64 m_seenVersion = ~0ull;
    std::string m_menuKey, m_viewportKey;
    bool m_keysHeld = false;
    f32 m_lastSyncMs = 0.f;
    bool m_frameTick = false;
    void syncPlayState_();
    void syncOrtho_();
    void orthoInput_(int i);
    bool orthoKey_(int i, fuse::ui::Key key, u8 mods);
    NodeHandle m_vpTopRow{}, m_vpBottomRow{}, m_ortho[3]{}, m_orthoCell[3]{};
    OrthoView m_orthoView[3];
    u32 m_orthoDrawn[3]{};
    std::string m_orthoKey[3];
    fuse::ui::Image m_orthoBg;
    u32 m_orthoBgIndex = 0;
    int m_layoutShown = -1;
    int m_playState = 0;
    bool m_maximized = false;
    f64 m_playAccumMs = 0.0;
    u32 m_playTicks = 0, m_playLastTick = 0;
    std::chrono::steady_clock::time_point m_playClock{};
    std::unique_ptr<EditorPanels> m_models;
    std::unique_ptr<class ShellPanelPages> m_pages;
};

/// ShellServices on FUSE UI dialogs (file dialog, message boxes). Each call runs a nested Application::step loop until
/// the dialog closes, like QFileDialog::getOpenFileName.
class DialogServices final : public ShellServices {
public:
    DialogServices(fuse::app::Application& app, fuse::app::AppWindow& window);
    std::optional<std::string> pickOpenFile(std::string_view title, std::string_view filter, std::string_view directory) override;
    std::optional<std::string> pickSaveFile(std::string_view title, std::string_view filter, std::string_view directory) override;
    std::optional<std::string> pickDirectory(std::string_view title, std::string_view directory) override;
    std::optional<Rgba> pickColour(Rgba initial) override;
    PromptAnswer askSaveDiscardCancel(std::string_view text) override;
    void warn(std::string_view title, std::string_view text) override;
    bool openExternal(std::string_view path) override;

private:
    std::optional<std::string> file_(std::string_view title, std::string_view filter, std::string_view directory, bool save);
    i32 buttons_(std::string_view title, std::string_view text, std::string_view buttons);
    fuse::app::Application& m_app;
    fuse::app::AppWindow& m_win;
};

} // namespace fuse::editor::fui
