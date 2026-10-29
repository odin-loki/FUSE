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
#include <fuse/editor/editor_panel_models.hpp>

#include "fuse/app/app.hpp"

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
    [[nodiscard]] u32 syncCount() const { return m_syncs; }
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
    u64 m_seenVersion = ~0ull;
    std::string m_menuKey, m_viewportKey;
    bool m_keysHeld = false;
    f32 m_lastSyncMs = 0.f;
    bool m_frameTick = false;
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

private:
    std::optional<std::string> file_(std::string_view title, std::string_view filter, std::string_view directory, bool save);
    i32 buttons_(std::string_view title, std::string_view text, std::string_view buttons);
    fuse::app::Application& m_app;
    fuse::app::AppWindow& m_win;
};

} // namespace fuse::editor::fui
