#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>

#include "game_loop_thread.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <mutex>
#include <vector>

class QAction;
class QCloseEvent;
class QDockWidget;
class QLabel;
class QMenu;
class QToolBar;

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
        /// Present the viewport through an embedded Vulkan child window (adopted FUSE VkInstance +
        /// real swapchain). Falls back to the software placeholder without a display / Vulkan WSI.
        bool embeddedVulkanViewport = true;
        /// VK_LAYER_KHRONOS_validation on the viewport renderer's instance (also
        /// FUSE_EDITOR_VK_VALIDATION=1).
        bool vulkanValidation = false;
        /// Keep File > Recent Files in QSettings (tests turn it off to stay hermetic).
        bool persistRecentFiles = true;
    };

    /// File menu action names (QAction::objectName), for `fileAction`.
    static constexpr const char* kActionNewScene = "fuseActionNewScene";
    static constexpr const char* kActionOpenScene = "fuseActionOpenScene";
    static constexpr const char* kActionSave = "fuseActionSaveScene";
    static constexpr const char* kActionSaveAs = "fuseActionSaveSceneAs";
    static constexpr const char* kActionNewProject = "fuseActionNewProject";
    static constexpr const char* kActionQuit = "fuseActionQuit";
    static constexpr int kMaxRecentFiles = 8;

    /// Answers the unsaved-changes prompt instead of a modal QMessageBox (tests): receives the
    /// prompt text, returns Save, Discard or Cancel.
    using DirtyPromptHook = std::function<QMessageBox::StandardButton(const QString& text)>;
    /// Answers the Open / Save As file dialogs instead of QFileDialog (tests): `saving` is true for
    /// Save As; return an empty string to cancel.
    using FileDialogHook = std::function<QString(bool saving)>;

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
    [[nodiscard]] ProjectHubWidget* projectHub() const { return m_projectHub; }
    [[nodiscard]] QDockWidget* dock(const char* objectName) const;
    [[nodiscard]] const std::vector<QDockWidget*>& docks() const { return m_docks; }

    /// Put every dock back where the default layout has it (viewport centre; hierarchy left,
    /// inspector right, console bottom — each raised above its tabified siblings).
    void resetToDefaultLayout();
    [[nodiscard]] QByteArray saveLayout() const;
    bool restoreLayout(const QByteArray& state);

    /// Refresh every panel from the FUSE models (runs on the UI refresh timer).
    void refreshPanels();

    /// Refresh the panels once the game thread has loaded an opened project's world (runs on the
    /// status timer). Returns true on the refresh that first shows the newly loaded world.
    bool syncProjectWorld();

    /// Duration of the last full repaint of the window (ms), as measured by `measureUiFrame`.
    double measureUiFrame();

    /// MP-B6-EDITOR-SCRIPT-PIE: Play / Pause / Resume / Step / Stop transport (toolbar + Play menu).
    /// Each action posts the matching command (StartPlay, PausePlay, ResumePlay, StepPlay, StopPlay)
    /// to the host's CommandQueue; the game tick drives PlaySession / PlayModeController.
    [[nodiscard]] QToolBar* transportToolBar() const { return m_transportBar; }
    [[nodiscard]] QAction* transportAction(CommandKind kind) const;
    /// Enable / disable the transport actions from EditorState (runs on the status timer).
    void updateTransportActions();

    // ---- UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES: File menu ----------------------------------------
    // New Scene / Open / Save / Save As / Recent Files / New Project / Quit call the E15 EditorHost
    // file commands (under the scene mutex, like every UI-side scene access). New / Open / Quit ask
    // to save unsaved changes (EditorHost::isSceneDirty: UndoStack / CommandStack / scene edits).
    // The window title shows the scene name and a '*' while dirty.

    [[nodiscard]] QAction* fileAction(const char* objectName) const;
    [[nodiscard]] QMenu* recentFilesMenu() const { return m_recentMenu; }
    [[nodiscard]] QStringList recentFiles() const { return m_recentFiles; }
    void setDirtyPromptHook(DirtyPromptHook hook) { m_dirtyPromptHook = std::move(hook); }
    void setFileDialogHook(FileDialogHook hook) { m_fileDialogHook = std::move(hook); }
    [[nodiscard]] int dirtyPromptCount() const { return m_dirtyPromptCount; }
    /// Asks to save when the scene is dirty. False when the user cancelled (or the save failed).
    bool maybeSaveChanges();
    /// File actions without their dirty prompt (the menu actions add it). Each returns the
    /// EditorHost result and refreshes panels, title and recent files.
    bool newScene();
    bool openScene(const QString& path);
    bool saveScene();
    bool saveSceneAs(const QString& path);
    /// Creates a project (E15 manifest writer, `EditorHost::newProject`) and opens it; `modules`
    /// are written into its project.json. Used by the Project hub's New Project wizard.
    bool createProject(const QString& directory, const QString& name, u32 dimensionFlags,
                       const project::ModuleSettings& modules);
    /// "<scene>[*] - <project> - FUSE Editor" from the host's current scene.
    void updateWindowTitle();
    [[nodiscard]] QString sceneDisplayName() const;

    // ---- Edit menu: one undo history over the host's UndoStack (structural edits) and
    // CommandStack (inspector property edits), in the order the edits happened.
    void undoLatest();
    void redoLatest();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class UndoSource : u8 { Scene, Property };
    void syncUndoJournal_();
    void addRecentFile(const QString& path);
    void rebuildRecentMenu();
    QString askFilePath(bool saving);
    void onFileNew();
    void onFileOpen();
    bool onFileSave();
    bool onFileSaveAs();
    void buildMenus();
    void buildTransport();
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
    QToolBar* m_transportBar = nullptr;
    QAction* m_actPlay = nullptr;
    QAction* m_actPause = nullptr;
    QAction* m_actResume = nullptr;
    QAction* m_actStep = nullptr;
    QAction* m_actStop = nullptr;
    QTimer m_statusTimer;
    bool m_projectWorldShown = false;
    QString m_projectLabel;
    QMenu* m_recentMenu = nullptr;
    QStringList m_recentFiles;
    DirtyPromptHook m_dirtyPromptHook;
    FileDialogHook m_fileDialogHook;
    int m_dirtyPromptCount = 0;
    u32 m_lastAppliedCommands = 0;
    std::vector<UndoSource> m_undoJournal;
    std::vector<UndoSource> m_redoJournal;
    u32 m_seenSceneUndo = 0;
    u32 m_seenPropertyUndo = 0;
};

} // namespace fuse::editor::qt
