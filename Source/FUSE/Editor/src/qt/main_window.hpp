#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>

#include "game_loop_thread.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <mutex>
#include <vector>

class QDockWidget;
class QLabel;

namespace fuse::editor::qt {

class AssetBrowserWidget;
class ConsoleWidget;
class HierarchyWidget;
class InspectorWidget;
class MaterialEditorWidget;
class ProfilerWidget;
class ProjectHubWidget;
class SdfSculptWidget;
class SeqPreviewPaneWidget;
class ViewportPlaceholderWidget;

/// FUSE Qt 6 editor shell (B6.1): `QMainWindow` + one `QDockWidget` per panel around the central
/// viewport. Every panel is bound to its Qt-free FUSE API object (`fuse::editor::*`); the game
/// thread ticks `EditorHost` under `sceneMutex()`, which the UI takes for every scene read / write.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    struct Options {
        bool startGameLoop = true;   ///< run EditorHost::gameTick on the game thread (~60 Hz)
        bool startFramePump = true;  ///< viewport camera frame pump (~60 Hz)
    };

    /// Dock object names (stable: they key QMainWindow::saveState / restoreState).
    static constexpr const char* kHierarchyDock = "dock.hierarchy";
    static constexpr const char* kInspectorDock = "dock.inspector";
    static constexpr const char* kConsoleDock = "dock.console";
    static constexpr const char* kProjectHubDock = "dock.project_hub";
    static constexpr const char* kAssetBrowserDock = "dock.asset_browser";
    static constexpr const char* kMaterialEditorDock = "dock.material_editor";
    static constexpr const char* kSdfSculptDock = "dock.sdf_sculpt";
    static constexpr const char* kProfilerDock = "dock.profiler";
    static constexpr const char* kSequencerDock = "dock.sequencer";

    explicit MainWindow(const QString& samplesRoot, QWidget* parent = nullptr);
    MainWindow(const QString& samplesRoot, const Options& options, QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorHost& host() { return m_host; }
    std::mutex& sceneMutex() { return m_sceneMutex; }

    [[nodiscard]] ViewportPlaceholderWidget* viewport() const { return m_viewport; }
    [[nodiscard]] HierarchyWidget* hierarchy() const { return m_hierarchy; }
    [[nodiscard]] InspectorWidget* inspector() const { return m_inspector; }
    [[nodiscard]] ConsoleWidget* console() const { return m_console; }
    [[nodiscard]] ProfilerWidget* profiler() const { return m_profiler; }
    [[nodiscard]] QDockWidget* dock(const char* objectName) const;
    [[nodiscard]] const std::vector<QDockWidget*>& docks() const { return m_docks; }

    /// Put every dock back where the default layout has it (viewport centre; hierarchy left,
    /// inspector right, console bottom — each raised above its tabified siblings).
    void resetToDefaultLayout();
    [[nodiscard]] QByteArray saveLayout() const;
    bool restoreLayout(const QByteArray& state);

    /// Refresh every panel from the FUSE models (runs on the UI refresh timer).
    void refreshPanels();

    /// Duration of the last full repaint of the window (ms), as measured by `measureUiFrame`.
    double measureUiFrame();

private:
    void buildMenus();
    void buildDocks();
    QDockWidget* addPanelDock(const char* objectName, const QString& title, QWidget* content);
    void onProjectOpenRequested(const QString& projectDirectory);
    void refreshStatusBar();
    void onFocusChanged(QWidget* old, QWidget* now);

    EditorHost m_host;
    FeaturePaneBridge m_featureBridge{m_host};
    std::mutex m_sceneMutex;
    GameLoopThread m_gameThread;
    Options m_options;
    ViewportPlaceholderWidget* m_viewport = nullptr;
    HierarchyWidget* m_hierarchy = nullptr;
    InspectorWidget* m_inspector = nullptr;
    ConsoleWidget* m_console = nullptr;
    ProjectHubWidget* m_projectHub = nullptr;
    AssetBrowserWidget* m_assetBrowser = nullptr;
    MaterialEditorWidget* m_materialEditor = nullptr;
    SdfSculptWidget* m_sdfSculpt = nullptr;
    ProfilerWidget* m_profiler = nullptr;
    SeqPreviewPaneWidget* m_sequencer = nullptr;
    std::vector<QDockWidget*> m_docks;
    QString m_samplesRoot;
    QLabel* m_statusLabel = nullptr;
    QTimer m_statusTimer;
};

} // namespace fuse::editor::qt
