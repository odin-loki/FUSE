#include "main_window.hpp"
#include <QSplitter>
#include "ortho_view_widget.hpp"
#include "preferences_dialog.hpp"
#include "navmesh_panel.hpp"
#include "terrain_panel.hpp"

#include <fuse/editor/runtime_viewport.hpp>
#include <QCoreApplication>
#include <QThread>
#include <QElapsedTimer>
#include "build_package_dialog.hpp"
#include "project_settings_dialog.hpp"

#include "editor_panels.hpp"
#include "project_hub_widget.hpp"
#include "property_pane_widget.hpp"
#include "seq_preview_pane_widget.hpp"
#include "timeline_widget.hpp"
#include "viewport_placeholder_widget.hpp"

#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/project/loader.hpp>

#include <QAction>
#include <QActionGroup>
#include <fuse/ecs/components/script.hpp>
#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <cstdlib>
#include <QTimer>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QStyle>
#include <QStatusBar>
#include <QToolBar>
#include <QVariant>

#include <algorithm>
#include <iterator>

namespace fuse::editor::qt {

MainWindow::MainWindow(const QString& samplesRoot, QWidget* parent) : MainWindow(samplesRoot, Options{}, parent) {}

MainWindow::MainWindow(const QString& samplesRoot, const Options& options, QWidget* parent)
    : QMainWindow(parent), m_gameThread(&m_host, &m_sceneMutex, this), m_options(options),
      m_samplesRoot(samplesRoot) {
    setObjectName(QStringLiteral("fuseEditorMainWindow"));
    setWindowTitle(tr("FUSE Editor"));
    // Inspector-editable module components (AudioSource) must be known before a scene loads.
    registerEditorComponentTypes();
    if (m_options.persistRecentFiles) {
        m_recentFiles = QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"))
                            .value(QStringLiteral("editor/recentScenes"))
                            .toStringList();
        while (m_recentFiles.size() > kMaxRecentFiles) {
            m_recentFiles.removeLast();
        }
        m_recentProjects = QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"))
                               .value(QStringLiteral("editor/recentProjects"))
                               .toStringList();
        while (m_recentProjects.size() > kMaxRecentFiles) {
            m_recentProjects.removeLast();
        }
    }
    // Default 1280x800 logical, clamped to the screen's available area (at high DPI scale factors
    // the default would otherwise exceed the screen and popups get re-clamped by the screen edge).
    QSize size(1280, 800);
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        size = size.boundedTo(avail.size());
        move(avail.topLeft());
    }
    resize(size);
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AnimatedDocks);
    setDockNestingEnabled(true);

    m_viewport = new ViewportPlaceholderWidget(m_host, m_sceneMutex, this);
    {
        // Multi-viewport container: [perspective | Top] over [Front | Side]; ortho views start hidden.
        auto* central = new QSplitter(Qt::Vertical, this);
        central->setObjectName(QStringLiteral("fuseViewportSplitter"));
        central->setChildrenCollapsible(false);
        m_viewTopRow = new QSplitter(Qt::Horizontal, central);
        m_viewTopRow->setChildrenCollapsible(false);
        m_viewBottomRow = new QSplitter(Qt::Horizontal, central);
        m_viewBottomRow->setChildrenCollapsible(false);
        m_viewTopRow->addWidget(m_viewport);
        using Axis = OrthoViewWidget::Axis;
        const Axis axes[3] = {Axis::Top, Axis::Front, Axis::Side};
        for (int i = 0; i < 3; ++i) {
            m_ortho[i] = new OrthoViewWidget(m_host, m_sceneMutex, axes[i]);
            (i == 0 ? m_viewTopRow : m_viewBottomRow)->addWidget(m_ortho[i]);
            m_ortho[i]->hide();
            connect(m_ortho[i], &OrthoViewWidget::selectionChanged, this, [this]() {
                m_inspector->refresh();
                m_viewport->update();
                refreshPanels();
            });
            connect(m_ortho[i], &OrthoViewWidget::sceneEdited, this, [this]() {
                m_viewport->update();
                refreshPanels();
            });
        }
        m_viewBottomRow->hide();
        central->addWidget(m_viewTopRow);
        central->addWidget(m_viewBottomRow);
        setCentralWidget(central);
    }

    buildDocks();
    buildTransport();
    buildMenus();
    buildTransformToolBar();
    resetToDefaultLayout();

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel, 1);

    connect(m_assetBrowser, &AssetBrowserWidget::sceneOpenRequested, this, [this](const QString& path) {
        if (path.endsWith(QStringLiteral(".prefab"), Qt::CaseInsensitive)) {
            // Double-clicking a prefab drops an instance into the open scene.
            m_host.postFromUi(makeInstantiatePrefabCommand(path.toStdString()));
            return;
        }
        if (maybeSaveChanges()) {
            openScene(path);
        }
    });
    connect(m_viewport, &ViewportPlaceholderWidget::sceneEdited, this, &MainWindow::refreshPanels);
    connect(m_viewport, &ViewportPlaceholderWidget::selectionChanged, this, &MainWindow::refreshPanels);
    connect(m_hierarchy, &HierarchyWidget::sceneEdited, this, &MainWindow::refreshPanels);
    connect(m_hierarchy, &HierarchyWidget::entityActivated, this, [this](ecs::EntityID id) {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            EditorState& st = m_host.editorState();
            if (std::find(st.selectedEntities.begin(), st.selectedEntities.end(), id) == st.selectedEntities.end()) {
                st.selectedEntities = {id};
                st.primarySelection = id;
            }
        }
        if (m_viewport != nullptr) (void)m_viewport->frameSelection();
    });
    connect(m_hierarchy, &HierarchyWidget::selectionChanged, this, [this]() {
        m_inspector->refresh();
        m_viewport->update();
    });
    // Inspector edits are posted to the game thread; the viewport repaints and the panels refresh
    // once the tick applied them (refreshStatusBar watches the applied-command count).
    connect(m_inspector, &InspectorWidget::propertyEdited, m_viewport, QOverload<>::of(&QWidget::update));
    connect(m_inspector, &InspectorWidget::componentsChanged, m_viewport, QOverload<>::of(&QWidget::update));
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::onFocusChanged);

    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatusBar);
    m_statusTimer.start();

    if (m_options.embeddedVulkanViewport) {
        // Before the game thread starts: its first tick creates the instance with the WSI extensions.
        m_viewport->enableEmbeddedVulkanViewport(m_options.vulkanValidation ||
                                                 qEnvironmentVariableIntValue("FUSE_EDITOR_VK_VALIDATION") != 0);
    }
    if (m_options.startGameLoop) {
        m_gameThread.start();
    }
    if (m_options.startFramePump) {
        m_viewport->setFramePumpEnabled(true);
    }
    for (QAction* menuAction : menuBar()->actions()) {
        if (menuAction->menu() != nullptr && menuAction->text() == tr("&Edit")) {
            menuAction->menu()->addSeparator();
            QAction* prefs = menuAction->menu()->addAction(tr("&Preferences..."));
            prefs->setObjectName(QStringLiteral("fuseActionPreferences"));
            prefs->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
            prefs->setMenuRole(QAction::PreferencesRole);
            connect(prefs, &QAction::triggered, this, [this]() { openPreferences(); });
            break;
        }
    }
    if (m_options.persistLayout) {
        const EditorPreferences prefs = EditorPreferences::load();
        setEditorPreviewSun(prefs.previewSun && std::getenv("FUSE_EDITOR_NO_PREVIEW_SUN") == nullptr);
        m_viewport->panel().camera().moveSpeed = static_cast<f32>(prefs.cameraSpeed);
        m_viewport->panel().camera().lookSensitivity = static_cast<f32>(prefs.lookSensitivity);
        applyShortcutOverrides(this);
        if (prefs.restoreLayout) restoreSettingsLayout();
    }
    refreshPanels();
    refreshStatusBar();
    updateWindowTitle();
}

void MainWindow::saveSettingsLayout() const {
    QSettings settings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"));
    settings.setValue(QString::fromLatin1(kSettingsGeometry), saveGeometry());
    settings.setValue(QString::fromLatin1(kSettingsLayout), saveLayout());
}

bool MainWindow::restoreSettingsLayout() {
    const QSettings settings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"));
    const QByteArray geometry = settings.value(QString::fromLatin1(kSettingsGeometry)).toByteArray();
    const QByteArray layout = settings.value(QString::fromLatin1(kSettingsLayout)).toByteArray();
    bool ok = false;
    if (!geometry.isEmpty()) {
        ok = restoreGeometry(geometry);
    }
    if (!layout.isEmpty()) {
        ok = restoreLayout(layout) || ok;
    }
    return ok;
}

MainWindow::~MainWindow() {
    disconnect(qApp, &QApplication::focusChanged, this, &MainWindow::onFocusChanged);
    m_statusTimer.stop();
    m_viewport->setFramePumpEnabled(false);
    m_gameThread.quit();
    m_gameThread.wait();
    // GPU work belongs to the UI thread again (tests tick the host there; teardown drains here).
    fuse::platform::registerRenderThread();
    // Vulkan teardown order: swapchain (game-side) -> Qt surface + adopted QVulkanInstance (UI) ->
    // FUSE VkInstance (with m_host, after this body). Child widgets outlive m_host, so the surface
    // must go here, not in the viewport's destructor.
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_host.runtimeViewport().releaseWindowSurface();
    }
    m_viewport->releaseVulkanViewport();
}

QDockWidget* MainWindow::addPanelDock(const char* objectName, const QString& title, QWidget* content) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(QString::fromLatin1(objectName));
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    dock->setWidget(content);
    m_docks.push_back(dock);
    return dock;
}

void MainWindow::buildDocks() {
    m_hierarchy = new HierarchyWidget(m_host, m_sceneMutex);
    m_inspector = new InspectorWidget(m_featureBridge, m_sceneMutex);
    m_console = new ConsoleWidget(&m_host);
    m_projectHub = new ProjectHubWidget();
    m_projectHub->setSamplesRoot(m_samplesRoot);
    m_assetBrowser = new AssetBrowserWidget();
    m_materialEditor = new MaterialEditorWidget(m_host, m_sceneMutex);
    m_sdfSculpt = new SdfSculptWidget(m_host, m_sceneMutex);
    m_profiler = new ProfilerWidget();
    m_sequencer = new SeqPreviewPaneWidget(m_host);
    m_timeline = new TimelineWidget(m_host, m_sceneMutex);

    addPanelDock(kHierarchyDock, tr("Hierarchy"), m_hierarchy);
    addPanelDock(kProjectHubDock, tr("Projects"), m_projectHub);
    addPanelDock(kAssetBrowserDock, tr("Asset Browser"), m_assetBrowser);
    addPanelDock(kInspectorDock, tr("Inspector"), m_inspector);
    addPanelDock(kMaterialEditorDock, tr("Material Editor"), m_materialEditor);
    connect(m_materialEditor, &MaterialEditorWidget::sceneEdited, this, [this]() {
        refreshPanels();
        updateEditActions();
    });
    addPanelDock(kSdfSculptDock, tr("SDF Sculpt"), m_sdfSculpt);
    m_viewport->setSculptPanel(&m_sdfSculpt->panel());
    connect(m_sdfSculpt, &SdfSculptWidget::sculptModeToggled, m_viewport, &ViewportPlaceholderWidget::setSculptMode);
    addPanelDock(kConsoleDock, tr("Console"), m_console);
    addPanelDock(kProfilerDock, tr("Profiler"), m_profiler);
    addPanelDock(kSequencerDock, tr("Sequencer"), m_sequencer);
    addPanelDock(kTimelineDock, tr("Timeline"), m_timeline);
    m_navigation = new NavmeshPanel(m_host, m_sceneMutex, m_viewport, this);
    addPanelDock(kNavigationDock, tr("Navigation"), m_navigation);
    m_terrain = new TerrainPanel(m_host, m_sceneMutex, m_viewport, this);
    addPanelDock(kTerrainDock, tr("Terrain"), m_terrain);
    // Sequencer <-> Timeline: one playhead while "Link to Timeline" is on.
    m_sequencer->setRangeMs(m_host.timeline().durationMs());
    connect(m_timeline, &TimelineWidget::playheadChanged, m_sequencer, [this](int ms) {
        m_sequencer->setRangeMs(m_host.timeline().durationMs());
        m_sequencer->followTimeline(ms);
    });
    connect(m_sequencer, &SeqPreviewPaneWidget::scrubbedTo, m_timeline, [this](int ms) {
        if (m_host.timeline().playheadMs() != ms) {
            m_timeline->scrubTo(ms);
        }
    });
    connect(m_timeline, &TimelineWidget::sceneEdited, this, [this]() {
        refreshPanels();
        updateEditActions();
    });

    connect(m_projectHub, &ProjectHubWidget::projectOpenRequested, this, &MainWindow::onProjectOpenRequested);
    connect(m_projectHub, &ProjectHubWidget::newProjectRequested, this, [this](const NewProjectRequest& request) {
        if (!maybeSaveChanges()) {
            return;
        }
        if (!createProject(request.directory, request.name, request.dimensionFlags, request.modules)) {
            const QString error = QString::fromStdString(m_host.lastFileResult().error);
            if (m_dirtyPromptHook == nullptr) { // interactive only: tests read lastFileResult()
                QMessageBox::warning(this, tr("New Project"), error);
            }
        }
    });
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    const auto addFileAction = [this, file](const QString& text, const char* objectName, QKeySequence shortcut) {
        QAction* action = file->addAction(text);
        action->setObjectName(QString::fromLatin1(objectName));
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        return action;
    };
    connect(addFileAction(tr("&New Scene"), kActionNewScene, QKeySequence::New), &QAction::triggered, this,
            &MainWindow::onFileNew);
    connect(addFileAction(tr("&Open Scene..."), kActionOpenScene, QKeySequence::Open), &QAction::triggered, this,
            &MainWindow::onFileOpen);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    m_recentMenu->setObjectName(QStringLiteral("fuseRecentFilesMenu"));
    rebuildRecentMenu();
    m_recentProjectsMenu = file->addMenu(tr("Recent &Projects"));
    m_recentProjectsMenu->setObjectName(QStringLiteral("fuseRecentProjectsMenu"));
    rebuildRecentProjectsMenu();
    file->addSeparator();
    connect(addFileAction(tr("&Save"), kActionSave, QKeySequence::Save), &QAction::triggered, this,
            &MainWindow::onFileSave);
    connect(addFileAction(tr("Save &As..."), kActionSaveAs, QKeySequence::SaveAs), &QAction::triggered, this,
            &MainWindow::onFileSaveAs);
    file->addSeparator();
    connect(addFileAction(tr("New &Project..."), kActionNewProject, QKeySequence()), &QAction::triggered,
            m_projectHub, &ProjectHubWidget::runNewProjectWizard);
    connect(addFileAction(tr("Open P&roject..."), kActionOpenProject, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)),
            &QAction::triggered, this, [this]() {
                if (!maybeSaveChanges()) {
                    return;
                }
                const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Project"), m_samplesRoot);
                if (!dir.isEmpty()) {
                    openProject(dir);
                }
            });
    QAction* settings = addFileAction(tr("Project Se&ttings..."), "fuseActionProjectSettings", QKeySequence());
    connect(settings, &QAction::triggered, this, [this]() { showProjectSettings(); });
    QAction* package = addFileAction(tr("&Build / Package..."), "fuseActionBuildPackage", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    connect(package, &QAction::triggered, this, [this]() {
        if (m_projectRoot.isEmpty()) {
            QMessageBox::information(this, tr("Build / Package"), tr("Open a project first."));
            return;
        }
        if (!maybeSaveChanges()) return;
        BuildPackageDialog dlg(m_projectRoot, this);
        dlg.exec();
    });
    QAction* import = addFileAction(tr("&Import Asset..."), "fuseActionImportAsset", QKeySequence(Qt::CTRL | Qt::Key_I));
    connect(import, &QAction::triggered, this, [this]() {
        if (m_projectRoot.isEmpty()) {
            QMessageBox::information(this, tr("Import Asset"), tr("Open a project first."));
            return;
        }
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Asset"), QString(),
            tr("Assets (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.ktx2 *.gltf *.glb *.obj *.wav *.ogg *.lua *.fuselevel *.prefab);;All files (*)"));
        if (!files.isEmpty()) {
            importAssets(files);
        }
    });
    file->addSeparator();
    QAction* quit = addFileAction(tr("&Quit"), kActionQuit, QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = edit->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("&Undo"));
    undo->setObjectName(QString::fromLatin1(kActionUndo));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, this, [this]() {
        undoLatest();
        updateEditActions();
    });
    QAction* redo = edit->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("&Redo"));
    redo->setObjectName(QString::fromLatin1(kActionRedo));
    redo->setShortcut(QKeySequence::Redo);
    connect(redo, &QAction::triggered, this, [this]() {
        redoLatest();
        updateEditActions();
    });
    m_actUndo = undo;
    m_actRedo = redo;
    edit->addSeparator();
    // Clipboard + multi-select (entity subtrees; one undo step each).
    auto addEditAction = [&](const QString& text, const char* name, const QKeySequence& key, auto fn) {
        QAction* a = edit->addAction(text);
        a->setObjectName(QString::fromLatin1(name));
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    auto afterEdit = [this]() {
        refreshPanels();
        if (m_viewport != nullptr) m_viewport->update();
    };
    m_actCut = addEditAction(tr("Cu&t"), "fuseActionCut", QKeySequence::Cut, [this, afterEdit]() {
        if (m_host.editorState().playing) return;
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_clipboard.cut(m_host.editorScene().registry(), m_host.undoStack(), m_host.editorState(), &m_host.entityNames());
        }
        afterEdit();
    });
    m_actCopy = addEditAction(tr("&Copy"), "fuseActionCopy", QKeySequence::Copy, [this]() {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        std::vector<ecs::EntityID> sel = m_host.editorState().selectedEntities;
        if (sel.empty() && m_host.editorState().primarySelection.valid()) sel.push_back(m_host.editorState().primarySelection);
        m_clipboard.copy(m_host.editorScene().registry(), sel, &m_host.entityNames());
    });
    m_actPaste = addEditAction(tr("&Paste"), "fuseActionPaste", QKeySequence::Paste, [this, afterEdit]() {
        if (m_host.editorState().playing) return;
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_clipboard.paste(m_host.editorScene().registry(), m_host.undoStack(), m_host.editorState(), &m_host.entityNames());
        }
        afterEdit();
    });
    edit->addSeparator();
    addEditAction(tr("Select &All"), "fuseActionSelectAll", QKeySequence::SelectAll, [this, afterEdit]() {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            EditorState& state = m_host.editorState();
            state.selectedEntities.clear();
            m_host.editorScene().registry().each<ecs::Transform>(
                [&](ecs::EntityID id, ecs::Transform&) { state.selectedEntities.push_back(id); });
            state.primarySelection = state.selectedEntities.empty() ? ecs::EntityID::null() : state.selectedEntities.front();
        }
        afterEdit();
    });
    addEditAction(tr("&Deselect All"), "fuseActionDeselectAll", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [this, afterEdit]() {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.editorState().selectedEntities.clear();
            m_host.editorState().primarySelection = ecs::EntityID::null();
        }
        afterEdit();
    });
    addEditAction(tr("&Find in Scene..."), "fuseActionFind", QKeySequence::Find, [this]() {
        if (m_hierarchy != nullptr) m_hierarchy->focusSearch();
    });
    connect(edit, &QMenu::aboutToShow, this, &MainWindow::updateEditActions);

    // File / Edit toolbar, left of the transport bar.
    m_mainBar = new QToolBar(tr("Main"), this);
    m_mainBar->setObjectName(QStringLiteral("fuseMainToolBar"));
    m_mainBar->setMovable(false);
    insertToolBar(m_transportBar, m_mainBar);
    const auto toolbarFileAction = [this](const char* objectName, QStyle::StandardPixmap icon) {
        if (QAction* action = fileAction(objectName)) {
            action->setIcon(style()->standardIcon(icon));
            m_mainBar->addAction(action);
        }
    };
    toolbarFileAction(kActionNewScene, QStyle::SP_FileIcon);
    toolbarFileAction(kActionOpenScene, QStyle::SP_DialogOpenButton);
    toolbarFileAction(kActionSave, QStyle::SP_DialogSaveButton);
    m_mainBar->addSeparator();
    m_mainBar->addAction(m_actUndo);
    m_mainBar->addAction(m_actRedo);
    updateEditActions();

    m_entityMenu = menuBar()->addMenu(tr("E&ntity"));
    m_entityMenu->setObjectName(QStringLiteral("fuseEntityMenu"));
    const auto addEntityAction = [this](const QString& text, const char* objectName, ContextMenuAction id,
                                        QKeySequence shortcut) {
        QAction* action = m_entityMenu->addAction(text);
        action->setObjectName(QString::fromLatin1(objectName));
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        connect(action, &QAction::triggered, this, [this, id]() { runEntityAction(id); });
        return action;
    };
    addEntityAction(tr("Create &Empty"), "fuseActionCreateEmpty", ContextMenuAction::CreateEmpty,
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    addEntityAction(tr("Create &Cube"), "fuseActionCreateCube", ContextMenuAction::CreateCube, {});
    addEntityAction(tr("Create SDF &Sphere"), "fuseActionCreateSdfSphere", ContextMenuAction::CreateSdfSphere, {});
    addEntityAction(tr("Create SDF &Box"), "fuseActionCreateSdfBox", ContextMenuAction::CreateSdfBox, {});
    addEntityAction(tr("Create &Point Light"), "fuseActionCreatePointLight", ContextMenuAction::CreatePointLight, {});
    addEntityAction(tr("Create &Directional Light"), "fuseActionCreateDirLight",
                    ContextMenuAction::CreateDirectionalLight, {});
    addEntityAction(tr("Create C&amera"), "fuseActionCreateCamera", ContextMenuAction::CreateCamera, {});
    m_entityMenu->addSeparator();
    {
        QAction* savePrefab = m_entityMenu->addAction(tr("Save Selection as &Prefab..."));
        savePrefab->setObjectName(QStringLiteral("fuseActionSavePrefab"));
        connect(savePrefab, &QAction::triggered, this, [this]() {
            const QString dir = m_projectRoot.isEmpty() ? QString() : m_projectRoot + QStringLiteral("/assets/prefabs");
            if (!dir.isEmpty()) QDir().mkpath(dir);
            const QString path = QFileDialog::getSaveFileName(this, tr("Save Prefab"), dir, tr("Prefabs (*.prefab)"));
            if (!path.isEmpty()) {
                m_host.postFromUi(makeSavePrefabCommand(path.endsWith(QStringLiteral(".prefab")) ? path.toStdString()
                                                                                               : (path + QStringLiteral(".prefab")).toStdString()));
            }
        });
        QAction* spawnPrefab = m_entityMenu->addAction(tr("&Instantiate Prefab..."));
        spawnPrefab->setObjectName(QStringLiteral("fuseActionInstantiatePrefab"));
        connect(spawnPrefab, &QAction::triggered, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this, tr("Instantiate Prefab"), m_projectRoot, tr("Prefabs (*.prefab)"));
            if (!path.isEmpty()) {
                m_host.postFromUi(makeInstantiatePrefabCommand(path.toStdString()));
            }
        });
        QAction* revertPrefab = m_entityMenu->addAction(tr("Revert Prefab Overrides"));
        revertPrefab->setObjectName(QStringLiteral("fuseActionRevertPrefab"));
        connect(revertPrefab, &QAction::triggered, this, [this]() { m_host.postFromUi(makePrefabLinkCommand("revert")); });
        QAction* reloadPrefab = m_entityMenu->addAction(tr("Update Prefab Instances"));
        reloadPrefab->setObjectName(QStringLiteral("fuseActionReloadPrefab"));
        connect(reloadPrefab, &QAction::triggered, this, [this]() { m_host.postFromUi(makePrefabLinkCommand("reload")); });
        QAction* unlinkPrefab = m_entityMenu->addAction(tr("Unlink Prefab Instance"));
        unlinkPrefab->setObjectName(QStringLiteral("fuseActionUnlinkPrefab"));
        connect(unlinkPrefab, &QAction::triggered, this, [this]() { m_host.postFromUi(makePrefabLinkCommand("unlink")); });
        m_entityMenu->addSeparator();
    }
    QAction* duplicate = addEntityAction(tr("D&uplicate"), "fuseActionDuplicate", ContextMenuAction::Duplicate,
                                         QKeySequence(Qt::CTRL | Qt::Key_D));
    QAction* remove = addEntityAction(tr("De&lete"), "fuseActionDelete", ContextMenuAction::Delete,
                                      QKeySequence::Delete);
    QAction* group = m_entityMenu->addAction(tr("&Group Selection"));
    group->setObjectName(QStringLiteral("fuseActionGroup"));
    group->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(group, &QAction::triggered, this, [this]() {
        if (m_host.editorState().playing) return;
        ecs::EntityID g;
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            g = groupSelection(m_host.editorScene().registry(), m_host.undoStack(), m_host.editorState(), &m_host.entityNames());
        }
        if (!g.valid()) {
            statusBar()->showMessage(tr("Group: select entities that share one parent"), 4000);
            return;
        }
        refreshPanels();
        if (m_viewport != nullptr) m_viewport->update();
    });
    QAction* ungroup = m_entityMenu->addAction(tr("U&ngroup"));
    ungroup->setObjectName(QStringLiteral("fuseActionUngroup"));
    ungroup->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(ungroup, &QAction::triggered, this, [this]() {
        if (m_host.editorState().playing) return;
        std::size_t moved = 0;
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            moved = ungroupSelection(m_host.editorScene().registry(), m_host.undoStack(), m_host.editorState());
        }
        if (moved == 0) {
            statusBar()->showMessage(tr("Ungroup: the selection has no children"), 4000);
            return;
        }
        refreshPanels();
        if (m_viewport != nullptr) m_viewport->update();
    });
    m_entityMenu->addSeparator();
    QAction* openScript = m_entityMenu->addAction(tr("Open &Script"));
    openScript->setObjectName(QStringLiteral("fuseActionOpenScript"));
    openScript->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    openScript->setToolTip(tr("Open the selected entity's Lua script in the system editor; saving it hot-reloads in Play"));
    connect(openScript, &QAction::triggered, this, [this]() {
        const QString path = selectedScriptPath();
        if (path.isEmpty()) {
            statusBar()->showMessage(tr("The selection has no Script component with a .lua file"), 3000);
            return;
        }
        if (m_fileDialogHook == nullptr) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        statusBar()->showMessage(tr("Opened %1 (changes hot-reload while playing)").arg(path), 4000);
    });
    connect(m_entityMenu, &QMenu::aboutToShow, this, [this, openScript]() { openScript->setEnabled(!selectedScriptPath().isEmpty()); });
    connect(m_entityMenu, &QMenu::aboutToShow, this, [this, duplicate, remove]() {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        const bool selected = m_host.editorState().primarySelection != ecs::EntityID::null();
        duplicate->setEnabled(selected);
        remove->setEnabled(selected);
    });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    QAction* colliders = view->addAction(tr("Show All &Colliders"));
    colliders->setObjectName(QStringLiteral("fuseActionShowColliders"));
    colliders->setCheckable(true);
    colliders->setToolTip(tr("Selected entities always show their collider wireframe"));
    connect(colliders, &QAction::toggled, this, [this](bool on) {
        if (m_viewport != nullptr) m_viewport->setAllCollidersVisible(on);
    });
    QAction* bounds = view->addAction(tr("Show Mesh &Bounds"));
    bounds->setObjectName(QStringLiteral("fuseActionShowMeshBounds"));
    bounds->setCheckable(true);
    connect(bounds, &QAction::toggled, this, [this](bool on) {
        if (m_viewport != nullptr) m_viewport->setMeshBoundsVisible(on);
    });
    QMenu* modes = view->addMenu(tr("View &Mode"));
    modes->setObjectName(QStringLiteral("fuseViewModeMenu"));
    auto* modeGroup = new QActionGroup(modes);
    const struct { const char* name; const char* text; EditorViewMode mode; QKeySequence key; } kModes[] = {
        {"fuseActionViewLit", "&Lit", EditorViewMode::Lit, QKeySequence(Qt::ALT | Qt::Key_1)},
        {"fuseActionViewUnlit", "&Unlit", EditorViewMode::Unlit, QKeySequence(Qt::ALT | Qt::Key_2)},
        {"fuseActionViewWireframe", "&Wireframe", EditorViewMode::Wireframe, QKeySequence(Qt::ALT | Qt::Key_3)},
        {"fuseActionViewLitWireframe", "Lit W&ireframe", EditorViewMode::LitWireframe, QKeySequence(Qt::ALT | Qt::Key_4)},
    };
    for (const auto& m : kModes) {
        QAction* a = modes->addAction(tr(m.text));
        a->setObjectName(QString::fromLatin1(m.name));
        a->setCheckable(true);
        a->setShortcut(m.key);
        a->setChecked(editorViewMode() == m.mode);
        modeGroup->addAction(a);
        const EditorViewMode mode = m.mode;
        connect(a, &QAction::triggered, this, [this, mode]() {
            setEditorViewMode(mode);
            if (m_viewport != nullptr) {
                m_viewport->syncOverlay(true);
                m_viewport->update();
            }
        });
    }
    QAction* frame = view->addAction(tr("&Frame Selected"));
    frame->setObjectName(QStringLiteral("fuseActionFrameSelected"));
    frame->setShortcut(QKeySequence(Qt::Key_F));
    frame->setShortcutContext(Qt::WindowShortcut);
    connect(frame, &QAction::triggered, this, [this]() {
        if (m_viewport != nullptr && !m_viewport->frameSelection()) statusBar()->showMessage(tr("Nothing selected to frame"), 3000);
    });
    view->addSeparator();
    for (QDockWidget* dock : m_docks) {
        view->addAction(dock->toggleViewAction());
    }
    view->addSeparator();
    QMenu* vpLayout = view->addMenu(tr("&Viewport Layout"));
    m_viewportLayoutGroup = new QActionGroup(this);
    const char* layoutNames[3] = {"fuseActionViewportSingle", "fuseActionViewportTwo", "fuseActionViewportQuad"};
    const QString layoutLabels[3] = {tr("&Single"), tr("&Two (Perspective + Top)"), tr("&Quad (Perspective, Top, Front, Side)")};
    for (int i = 0; i < 3; ++i) {
        QAction* a = vpLayout->addAction(layoutLabels[i]);
        a->setObjectName(QString::fromLatin1(layoutNames[i]));
        a->setCheckable(true);
        a->setChecked(i == 0);
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | static_cast<Qt::Key>(Qt::Key_1 + i))); // Alt+1..4 = view modes
        m_viewportLayoutGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, i]() { setViewportLayout(i); });
    }
    QAction* reset = view->addAction(tr("Reset &Layout"));
    connect(reset, &QAction::triggered, this, &MainWindow::resetToDefaultLayout);

    QMenu* play = menuBar()->addMenu(tr("&Play"));
    play->addAction(m_actPlay);
    play->addAction(m_actPause);
    play->addAction(m_actResume);
    play->addAction(m_actStep);
    play->addAction(m_actStop);
    play->addSeparator();
    m_actMaximizeOnPlay = play->addAction(tr("&Maximize Viewport on Play"));
    m_actMaximizeOnPlay->setObjectName(QStringLiteral("fuseActionMaximizeOnPlay"));
    m_actMaximizeOnPlay->setCheckable(true);
    m_actMaximizeOnPlay->setToolTip(tr("Hide the docks while playing; Stop restores the layout"));
    QAction* fromSpawn = play->addAction(tr("Play from &Spawn Point"));
    fromSpawn->setObjectName(QStringLiteral("fuseActionPlayFromSpawn"));
    fromSpawn->setCheckable(true);
    fromSpawn->setToolTip(tr("Start Play at the first spawn point instead of the current editor view"));
    connect(fromSpawn, &QAction::toggled, this, [this](bool on) { m_host.runtimeViewport().setPlayFromSpawn(on); });
}

void MainWindow::setViewportLayout(int layout) {
    layout = std::clamp(layout, 0, 2);
    m_viewportLayout = layout;
    m_ortho[0]->setVisible(layout >= 1);
    m_ortho[1]->setVisible(layout == 2);
    m_ortho[2]->setVisible(layout == 2);
    m_viewBottomRow->setVisible(layout == 2);
    if (layout >= 1) {
        const int w = std::max(2, m_viewTopRow->width());
        m_viewTopRow->setSizes({w / 2, w / 2});
        auto* central = qobject_cast<QSplitter*>(centralWidget());
        if (layout == 2 && central != nullptr) {
            const int h = std::max(2, central->height());
            central->setSizes({h / 2, h / 2});
            m_viewBottomRow->setSizes({w / 2, w / 2});
        }
        // Each ortho view frames the whole scene the first time it gets a size (OrthoViewWidget::resizeEvent).
    }
    if (m_viewportLayoutGroup != nullptr) {
        const auto actions = m_viewportLayoutGroup->actions();
        if (layout < actions.size()) actions[layout]->setChecked(true);
    }
}

void MainWindow::setMaximizeOnPlay(bool on) {
    if (m_actMaximizeOnPlay != nullptr) m_actMaximizeOnPlay->setChecked(on);
}

bool MainWindow::maximizeOnPlay() const { return m_actMaximizeOnPlay != nullptr && m_actMaximizeOnPlay->isChecked(); }

qint64 MainWindow::playElapsedMs() const {
    return m_playAccumMs + (m_wasPlaying && !m_wasPaused && m_playClock.isValid() ? m_playClock.elapsed() : 0);
}

void MainWindow::onPlayStateChanged_(bool playing, bool paused) {
    const bool startedPlay = playing && !m_wasPlaying;
    const bool stoppedPlay = !playing && m_wasPlaying;
    if (startedPlay) {
        m_playAccumMs = 0;
        m_playTicks = 0;
        m_playLastTick = m_host.gameTickCount();
        if (maximizeOnPlay()) {
            m_prePlayState = saveState();
            for (QDockWidget* d : m_docks) d->hide();
        }
    }
    // Clock runs only while unpaused.
    if (m_wasPlaying && !m_wasPaused && m_playClock.isValid()) {
        m_playAccumMs += m_playClock.elapsed();
        m_playClock.invalidate();
    }
    if (playing && !paused) m_playClock.start();
    if (stoppedPlay && !m_prePlayState.isEmpty()) {
        restoreState(m_prePlayState);
        m_prePlayState.clear();
    }
    m_wasPlaying = playing;
    m_wasPaused = paused;
}

void MainWindow::buildTransformToolBar() {
    // GUI-2: viewport transform tools. Plain W/E/R would collide with WASD fly, so tools use Ctrl+1..4.
    m_transformBar = new QToolBar(tr("Transform"), this);
    m_transformBar->setObjectName(QStringLiteral("fuseTransformToolBar"));
    m_transformBar->setMovable(false);
    m_transformBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    insertToolBar(m_transportBar, m_transformBar);
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    const auto addTool = [this, group](const QString& text, const char* objectName, TransformTool tool, int key,
                                       const QString& tip) {
        auto* action = new QAction(text, this);
        action->setObjectName(QString::fromLatin1(objectName));
        action->setCheckable(true);
        action->setShortcut(QKeySequence(Qt::CTRL | key));
        action->setToolTip(tip + QStringLiteral(" (%1)").arg(action->shortcut().toString(QKeySequence::NativeText)));
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this, [this, tool]() { m_viewport->setTransformTool(tool); });
        m_transformBar->addAction(action);
        return action;
    };
    addTool(tr("Select"), "fuseActionToolSelect", TransformTool::Select, Qt::Key_1, tr("Select / pick only"));
    QAction* translate =
        addTool(tr("Move"), "fuseActionToolTranslate", TransformTool::Translate, Qt::Key_2, tr("Translate along an axis"));
    addTool(tr("Rotate"), "fuseActionToolRotate", TransformTool::Rotate, Qt::Key_3, tr("Rotate about an axis"));
    addTool(tr("Scale"), "fuseActionToolScale", TransformTool::Scale, Qt::Key_4, tr("Scale along a local axis"));
    translate->setChecked(true);
    m_viewport->setTransformTool(TransformTool::Translate);
    m_transformBar->addSeparator();
    auto* local = new QAction(tr("Local"), this);
    local->setObjectName(QStringLiteral("fuseActionSpaceLocal"));
    local->setCheckable(true);
    local->setToolTip(tr("Use the entity's local axes (off = world axes)"));
    connect(local, &QAction::toggled, this, [this](bool on) {
        m_viewport->setTransformSpace(on ? TransformSpace::Local : TransformSpace::World);
    });
    m_transformBar->addAction(local);
    auto* snap = new QAction(tr("Snap"), this);
    snap->setObjectName(QStringLiteral("fuseActionSnap"));
    snap->setCheckable(true);
    snap->setToolTip(tr("Snap: 0.5 units / 15° / 0.1× scale"));
    connect(snap, &QAction::toggled, this, [this](bool on) { m_viewport->setTransformSnap(on); });
    m_transformBar->addAction(snap);
}

void MainWindow::buildTransport() {
    m_transportBar = addToolBar(tr("Play"));
    m_transportBar->setObjectName(QStringLiteral("fuseTransportToolBar"));
    m_transportBar->setMovable(false);
    const auto makeAction = [this](const QString& text, const char* objectName, CommandKind kind,
                                   QStyle::StandardPixmap icon) {
        auto* action = new QAction(style()->standardIcon(icon), text, this);
        action->setObjectName(QString::fromLatin1(objectName));
        connect(action, &QAction::triggered, this, [this, kind]() {
            m_host.postFromUi(makeTransportCommand(kind));
            updateTransportActions();
        });
        m_transportBar->addAction(action);
        return action;
    };
    m_actPlay = makeAction(tr("&Play"), "fuseActionPlay", CommandKind::StartPlay, QStyle::SP_MediaPlay);
    m_actPlay->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    m_actPause = makeAction(tr("P&ause"), "fuseActionPause", CommandKind::PausePlay, QStyle::SP_MediaPause);
    m_actResume = makeAction(tr("&Resume"), "fuseActionResume", CommandKind::ResumePlay, QStyle::SP_MediaSeekForward);
    m_actStep = makeAction(tr("S&tep"), "fuseActionStep", CommandKind::StepPlay, QStyle::SP_MediaSkipForward);
    m_actStep->setShortcut(QKeySequence(Qt::Key_F10));
    // PIE shortcuts: F5 plays (or resumes when paused), Shift+F5 stops, F6 pauses.
    m_actPlay->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_P), QKeySequence(Qt::Key_F5)});
    m_actResume->setShortcut(QKeySequence(Qt::Key_F5));
    m_actPause->setShortcut(QKeySequence(Qt::Key_F6));
    m_actStop = makeAction(tr("&Stop"), "fuseActionStop", CommandKind::StopPlay, QStyle::SP_MediaStop);
    m_actStop->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    updateTransportActions();
}

QAction* MainWindow::transportAction(CommandKind kind) const {
    switch (kind) {
    case CommandKind::StartPlay:
        return m_actPlay;
    case CommandKind::PausePlay:
        return m_actPause;
    case CommandKind::ResumePlay:
        return m_actResume;
    case CommandKind::StepPlay:
        return m_actStep;
    case CommandKind::StopPlay:
        return m_actStop;
    default:
        return nullptr;
    }
}

void MainWindow::updateEditActions() {
    if (m_actUndo == nullptr) {
        return;
    }
    bool canUndo = false;
    bool canRedo = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        canUndo = m_host.undoStack().canUndo() || m_host.commandStack().canUndo();
        canRedo = m_host.undoStack().canRedo() || m_host.commandStack().canRedo();
    }
    m_actUndo->setEnabled(canUndo);
    m_actRedo->setEnabled(canRedo);
    if (m_actPaste != nullptr) {
        const bool playing = m_host.editorState().playing;
        m_actPaste->setEnabled(!m_clipboard.empty() && !playing);
        m_actCut->setEnabled(!playing);
    }
}

bool MainWindow::runEntityAction(ContextMenuAction action) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorState& state = m_host.editorState();
        const bool needsSelection = action == ContextMenuAction::Duplicate || action == ContextMenuAction::Delete;
        if (needsSelection && state.primarySelection == ecs::EntityID::null()) {
            return false;
        }
        if (action == ContextMenuAction::Duplicate && !state.playing) {
            // Whole subtrees and their display names, one undo step, copies selected (the context-menu
            // model's Duplicate copies only the selected entities themselves).
            std::vector<ecs::EntityID> selection = state.selectedEntities;
            if (selection.empty()) selection.push_back(state.primarySelection);
            EntityClipboard scratch;
            ecs::Registry& registry = m_host.editorScene().registry();
            if (scratch.copy(registry, selection, &m_host.entityNames()) != 0u) {
                changed = !scratch.paste(registry, m_host.undoStack(), state, &m_host.entityNames()).empty();
            }
        } else {
        m_entityActions.openAt(state.primarySelection, ecs::vec3{0.f, 0.f, 0.f}, state);
        changed = m_entityActions.activate(action, m_host.editorScene(), state, m_host.undoStack());
        m_entityActions.close();
        }
    }
    if (changed) {
        refreshPanels();
        updateEditActions();
        updateWindowTitle();
        m_viewport->update();
    }
    return changed;
}

void MainWindow::openProject(const QString& projectDirectory) {
    onProjectOpenRequested(projectDirectory);
}

void MainWindow::updateTransportActions() {
    if (m_actPlay == nullptr) {
        return;
    }
    const EditorState& state = m_host.editorState();
    const bool playing = state.playing;
    const bool paused = state.playing && state.paused;
    if (playing != m_wasPlaying || paused != m_wasPaused) onPlayStateChanged_(playing, paused);
    if (playing) {
        const quint64 tick = m_host.gameTickCount();
        if (!paused || tick != m_playLastTick) m_playTicks += tick - std::min(tick, m_playLastTick); // Step counts too
        m_playLastTick = tick;
        const qint64 ms = playElapsedMs();
        m_viewport->setPlayBanner(paused ? 2 : 1,
                                  (paused ? tr("PAUSED") : tr("PLAYING")) +
                                      QStringLiteral("  %1 s  ·  %2 ticks").arg(ms / 1000.0, 0, 'f', 1).arg(m_playTicks));
    } else {
        m_viewport->setPlayBanner(0, QString());
    }
    m_actPlay->setEnabled(!playing);
    m_actPause->setEnabled(playing && !paused);
    m_actResume->setEnabled(paused);
    m_actStep->setEnabled(paused);
    m_actStop->setEnabled(playing);
    // PIE polish: scene file and entity-authoring commands act on the edit scene, so they are
    // disabled while Play runs (the play registry is a copy); the viewport dock shows the state.
    for (const char* name : {kActionSave, kActionSaveAs, "fuseActionSavePrefab", "fuseActionInstantiatePrefab", "fuseActionRevertPrefab",
                             "fuseActionReloadPrefab", "fuseActionUnlinkPrefab"}) {
        if (QAction* a = findChild<QAction*>(QString::fromLatin1(name))) {
            a->setEnabled(!playing);
        }
    }
    if (m_entityMenu != nullptr) {
        m_entityMenu->setEnabled(!playing);
    }
    if (m_statusLabel != nullptr) {
        const QString style = !playing ? QString()
                              : QStringLiteral("QLabel { background: %1; color: white; padding: 0 6px; }")
                                    .arg(paused ? QStringLiteral("#7a5a12") : QStringLiteral("#1f6e3a"));
        if (m_statusLabel->styleSheet() != style) {
            m_statusLabel->setStyleSheet(style);
        }
    }
}

QDockWidget* MainWindow::dock(const char* objectName) const {
    for (QDockWidget* d : m_docks) {
        if (d->objectName() == QLatin1String(objectName)) {
            return d;
        }
    }
    return nullptr;
}

void MainWindow::resetToDefaultLayout() {
    for (QDockWidget* d : m_docks) {
        d->setFloating(false);
        removeDockWidget(d);
    }
    QDockWidget* hierarchyDock = dock(kHierarchyDock);
    QDockWidget* projectDock = dock(kProjectHubDock);
    QDockWidget* assetDock = dock(kAssetBrowserDock);
    QDockWidget* inspectorDock = dock(kInspectorDock);
    QDockWidget* materialDock = dock(kMaterialEditorDock);
    QDockWidget* sdfDock = dock(kSdfSculptDock);
    QDockWidget* consoleDock = dock(kConsoleDock);
    QDockWidget* profilerDock = dock(kProfilerDock);
    QDockWidget* sequencerDock = dock(kSequencerDock);
    QDockWidget* timelineDock = dock(kTimelineDock);
    QDockWidget* navigationDock = dock(kNavigationDock);
    QDockWidget* terrainDock = dock(kTerrainDock);

    // Bottom spans the full width; left / right columns sit beside the viewport above it.
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    addDockWidget(Qt::LeftDockWidgetArea, hierarchyDock);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);
    splitDockWidget(hierarchyDock, projectDock, Qt::Vertical);
    tabifyDockWidget(projectDock, assetDock);

    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
    tabifyDockWidget(inspectorDock, materialDock);
    tabifyDockWidget(inspectorDock, sdfDock);
    tabifyDockWidget(inspectorDock, navigationDock);
    tabifyDockWidget(inspectorDock, terrainDock);

    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    tabifyDockWidget(consoleDock, profilerDock);
    tabifyDockWidget(consoleDock, sequencerDock);
    tabifyDockWidget(consoleDock, timelineDock);

    for (QDockWidget* d : m_docks) {
        d->show();
    }
    projectDock->raise();
    inspectorDock->raise();
    consoleDock->raise();

    resizeDocks({hierarchyDock, inspectorDock}, {260, 320}, Qt::Horizontal);
    resizeDocks({consoleDock}, {200}, Qt::Vertical);
    resizeDocks({hierarchyDock, projectDock}, {300, 200}, Qt::Vertical);
}

QByteArray MainWindow::saveLayout() const {
    return saveState();
}

bool MainWindow::restoreLayout(const QByteArray& state) {
    return restoreState(state);
}

void MainWindow::onProjectOpenRequested(const QString& projectDirectory) {
    const QFileInfo info(projectDirectory);
    const QString root = info.absoluteFilePath();
    m_viewport->setProjectLabel(info.fileName());
    m_assetBrowser->setProjectRoot(root);
    m_projectRoot = root;
    m_projectLabel = info.fileName();
    updateWindowTitle();
    addRecentProject(root);

    // `project.root` switches the scene and makes the next game tick load the project's default
    // world (EditorHost -> RuntimeViewportHook::ensureWorldLoaded_); `project` sets the label.
    // refreshStatusBar() refreshes the panels once the world is in.
    EditorCommand rootCmd;
    rootCmd.kind = CommandKind::SetProperty;
    rootCmd.propertyName = "project.root";
    rootCmd.propertyValue = root.toStdString();
    m_host.postFromUi(std::move(rootCmd));

    EditorCommand cmd;
    cmd.kind = CommandKind::SetProperty;
    cmd.propertyName = "project";
    cmd.propertyValue = info.fileName().toStdString();
    m_host.postFromUi(std::move(cmd));
    m_projectWorldShown = false;
}

bool MainWindow::syncProjectWorld() {
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        loaded = m_host.runtimeViewport().embedSession().worldLoaded;
    }
    if (loaded && !m_projectWorldShown) {
        m_projectWorldShown = true;
        refreshPanels();
        return true;
    }
    if (!loaded) {
        m_projectWorldShown = false;
    }
    return false;
}

void MainWindow::refreshPanels() {
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        syncUndoJournal_(); // keep Edit > Undo's cross-stack order current
    }
    m_hierarchy->refresh();
    m_inspector->refresh();
    m_materialEditor->refresh();
    m_sdfSculpt->refresh();
    m_timeline->refresh();
    m_viewport->update();
}

void MainWindow::onFocusChanged(QWidget* /*old*/, QWidget* now) {
    // TitleBgActive for the dock that holds keyboard focus, TitleBg for the others.
    for (QDockWidget* d : m_docks) {
        const bool active = now != nullptr && d->isAncestorOf(now);
        if (d->property("fuseActive").toBool() != active) {
            d->setProperty("fuseActive", active);
            d->style()->unpolish(d);
            d->style()->polish(d);
            d->update();
        }
    }
}

double MainWindow::measureUiFrame() {
    QElapsedTimer timer;
    timer.start();
    repaint();
    const double ms = static_cast<double>(timer.nsecsElapsed()) * 1e-6;
    ProfilerPanel::FrameProfileData frame{};
    frame.cpuMs = static_cast<f32>(ms);
    frame.postprocessMs = static_cast<f32>(ms);
    m_profiler->pushFrame(frame);
    return ms;
}

void MainWindow::refreshStatusBar() {
    // The 100 ms status refresh must never stall the UI thread behind a long game tick (the first
    // Lavapipe bring-up / pipeline build holds the scene mutex for seconds): skip this round when
    // the game thread owns the scene; the next timer tick catches up.
    if (m_gameThread.isRunning()) {
        std::unique_lock<std::mutex> probe(m_sceneMutex, std::try_to_lock);
        if (!probe.owns_lock()) {
            return;
        }
    }
    syncProjectWorld();
    m_console->drainLog();
    updateTransportActions();
    updateEditActions();
    u32 applied = 0;
    QString prefabInfo;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        syncUndoJournal_();
        applied = m_host.commandQueue().appliedCount();
        const ecs::EntityID selected = m_host.editorState().primarySelection;
        if (const auto link = m_host.prefabLinkOf(selected)) {
            const QString name = QFileInfo(QString::fromStdString(link->path)).fileName();
            prefabInfo = tr(" | Prefab: %1 (%2 override(s))").arg(name).arg(link->overrideCount);
        }
    }
    if (applied != m_lastAppliedCommands) {
        // The game tick applied posted commands (inspector edits, add / remove, reparent).
        m_lastAppliedCommands = applied;
        refreshPanels();
    } else {
        m_inspector->refresh(); // cheap when nothing changed; picks up gizmo / PIE edits
    }
    updateWindowTitle();
    if (m_gameThread.isRunning() && m_host.gameTickCount() != m_lastProfiledTick) {
        // One profiler sample per status refresh: the game thread's last tick wall time.
        m_lastProfiledTick = m_host.gameTickCount();
        ProfilerPanel::FrameProfileData frame{};
        frame.cpuMs = m_gameThread.lastTickMs();
        m_profiler->pushFrame(frame);
    }
    m_profiler->refresh();
    m_statusLabel->setText(
        tr("Game ticks: %1 | Pending: %2 | Applied: %3 | PIE: %4%5")
            .arg(m_host.gameTickCount())
            .arg(m_host.commandQueue().pendingCount())
            .arg(m_host.commandQueue().appliedCount())
            .arg(!m_host.editorState().playing ? tr("off")
                 : m_host.editorState().paused ? tr("paused")
                                               : tr("playing"))
            .arg(prefabInfo));
    updateTransportActions();
    for (OrthoViewWidget* o : m_ortho) {
        if (o != nullptr && o->isVisible()) o->update();
    }
}

} // namespace fuse::editor::qt

namespace fuse::editor::qt {

// ---- File menu (UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES) ----------------------------------------------

QAction* MainWindow::fileAction(const char* objectName) const {
    return findChild<QAction*>(QString::fromLatin1(objectName));
}

QString MainWindow::sceneDisplayName() const {
    const std::string& path = m_host.currentScenePath();
    if (!path.empty()) {
        return QFileInfo(QString::fromStdString(path)).completeBaseName();
    }
    const std::string& name = m_host.runtimeScene().name();
    return name.empty() ? tr("Untitled") : QString::fromStdString(name);
}

void MainWindow::updateWindowTitle() {
    QString scene;
    bool dirty = false;
    QString project = m_projectLabel;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        scene = sceneDisplayName();
        dirty = m_host.isSceneDirty();
        if (m_host.hasProject() && !m_host.projectManifest().name.empty()) {
            project = QString::fromStdString(m_host.projectManifest().name);
        }
    }
    QString title = scene + (dirty ? QStringLiteral("*") : QString());
    if (!project.isEmpty()) {
        title += QStringLiteral(" - ") + project;
    }
    if (m_host.editorState().playing) {
        title += m_host.editorState().paused ? tr(" [Paused]") : tr(" [Playing]");
    }
    title += QStringLiteral(" - ") + tr("FUSE Editor");
    if (windowTitle() != title) {
        setWindowTitle(title);
    }
}

bool MainWindow::maybeSaveChanges() {
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        dirty = m_host.isSceneDirty();
    }
    if (!dirty) {
        return true;
    }
    ++m_dirtyPromptCount;
    QString scene;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        scene = sceneDisplayName();
    }
    const QString text = tr("The scene \"%1\" has unsaved changes. Save them?").arg(scene);
    QMessageBox::StandardButton answer = QMessageBox::Cancel;
    if (m_dirtyPromptHook != nullptr) {
        answer = m_dirtyPromptHook(text);
    } else {
        answer = QMessageBox::question(this, tr("Unsaved Changes"), text,
                                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                       QMessageBox::Save);
    }
    if (answer == QMessageBox::Save) {
        return onFileSave();
    }
    return answer == QMessageBox::Discard;
}

QString MainWindow::askFilePath(bool saving) {
    if (m_fileDialogHook != nullptr) {
        return m_fileDialogHook(saving);
    }
    QString directory;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        if (!m_host.currentScenePath().empty()) {
            directory = QFileInfo(QString::fromStdString(m_host.currentScenePath())).absolutePath();
        } else if (m_host.hasProject()) {
            directory = QString::fromStdString(m_host.projectManifest().projectRoot);
        }
    }
    const QString filter = tr("FUSE level (*.fuselevel)");
    if (!saving) {
        return QFileDialog::getOpenFileName(this, tr("Open Scene"), directory, filter);
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Scene As"), directory, filter);
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".fuselevel");
    }
    return path;
}

bool MainWindow::newScene() {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        // Same dimension as the current scene (a 2D project gets a 2D scene).
        ok = m_host.newScene(m_host.currentSceneDimension());
        syncUndoJournal_();
    }
    refreshPanels();
    updateWindowTitle();
    return ok;
}

bool MainWindow::openScene(const QString& path) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ok = m_host.openScene(path.toStdString());
        syncUndoJournal_();
    }
    if (ok) {
        addRecentFile(path);
    }
    refreshPanels();
    updateWindowTitle();
    return ok;
}

bool MainWindow::saveScene() {
    bool ok = false;
    QString path;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ok = m_host.saveScene();
        path = QString::fromStdString(m_host.currentScenePath());
    }
    if (ok) {
        addRecentFile(path);
    }
    updateWindowTitle();
    return ok;
}

bool MainWindow::saveSceneAs(const QString& path) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ok = m_host.saveSceneAs(path.toStdString());
    }
    if (ok) {
        addRecentFile(path);
    }
    updateWindowTitle();
    return ok;
}

bool MainWindow::createProject(const QString& directory, const QString& name, u32 dimensionFlags,
                               const project::ModuleSettings& modules) {
    bool ok = false;
    std::string root;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ok = m_host.newProject(directory.toStdString(), name.toStdString(), dimensionFlags);
        if (ok) {
            project::ProjectManifest manifest = m_host.projectManifest();
            root = manifest.projectRoot;
            const project::ModuleSettings& old = manifest.modules;
            if (old.ai != modules.ai || old.cinematics != modules.cinematics || old.fx != modules.fx ||
                old.mechanics != modules.mechanics || old.adventure != modules.adventure) {
                // Module toggles go into project.json through the same manifest writer, then the
                // project is reopened so the host's manifest matches the file.
                manifest.modules = modules;
                const project::SaveResult saved = project::saveToDirectory(manifest, root);
                ok = saved.ok && m_host.openProject(root);
            }
        }
        syncUndoJournal_();
    }
    if (ok) {
        const QString label = QString::fromStdString(m_host.projectManifest().name);
        m_projectLabel = label;
        m_viewport->setProjectLabel(label);
        m_assetBrowser->setProjectRoot(QString::fromStdString(root));
        m_projectRoot = QString::fromStdString(root);
        m_projectHub->addProjectDirectory(QString::fromStdString(root));
        if (!m_host.currentScenePath().empty()) {
            addRecentFile(QString::fromStdString(m_host.currentScenePath()));
        }
    }
    refreshPanels();
    updateWindowTitle();
    return ok;
}

void MainWindow::onFileNew() {
    if (maybeSaveChanges()) {
        newScene();
    }
}

void MainWindow::onFileOpen() {
    if (!maybeSaveChanges()) {
        return;
    }
    const QString path = askFilePath(false);
    if (!path.isEmpty() && !openScene(path) && m_fileDialogHook == nullptr) {
        QMessageBox::warning(this, tr("Open Scene"), QString::fromStdString(m_host.lastFileResult().error));
    }
}

bool MainWindow::onFileSave() {
    bool hasPath = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        hasPath = !m_host.currentScenePath().empty();
    }
    if (!hasPath) {
        return onFileSaveAs();
    }
    const bool ok = saveScene();
    if (!ok && m_fileDialogHook == nullptr) {
        QMessageBox::warning(this, tr("Save Scene"), QString::fromStdString(m_host.lastFileResult().error));
    }
    return ok;
}

bool MainWindow::onFileSaveAs() {
    const QString path = askFilePath(true);
    if (path.isEmpty()) {
        return false;
    }
    const bool ok = saveSceneAs(path);
    if (!ok && m_fileDialogHook == nullptr) {
        QMessageBox::warning(this, tr("Save Scene As"), QString::fromStdString(m_host.lastFileResult().error));
    }
    return ok;
}

void MainWindow::addRecentFile(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    m_recentFiles.removeAll(absolute);
    m_recentFiles.prepend(absolute);
    while (m_recentFiles.size() > kMaxRecentFiles) {
        m_recentFiles.removeLast();
    }
    if (m_options.persistRecentFiles) {
        QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"))
            .setValue(QStringLiteral("editor/recentScenes"), m_recentFiles);
    }
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (m_recentMenu == nullptr) {
        return;
    }
    m_recentMenu->clear();
    int index = 1;
    for (const QString& path : m_recentFiles) {
        QAction* action = m_recentMenu->addAction(QStringLiteral("&%1 %2").arg(index++).arg(QDir::toNativeSeparators(path)));
        action->setData(path);
        connect(action, &QAction::triggered, this, [this, path]() {
            if (maybeSaveChanges() && !openScene(path) && m_fileDialogHook == nullptr) {
                QMessageBox::warning(this, tr("Open Recent"), QString::fromStdString(m_host.lastFileResult().error));
            }
        });
    }
    m_recentMenu->setEnabled(!m_recentFiles.isEmpty());
}

QString MainWindow::selectedScriptPath() {
    std::string path;
    std::string root;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        const ecs::EntityID id = m_host.editorState().primarySelection;
        if (id == ecs::EntityID::null()) return {};
        const ecs::Script* script = m_host.editorScene().registry().get<ecs::Script>(id);
        if (script == nullptr || script->path().empty()) return {};
        path = std::string(script->path());
        root = m_host.playSession().scriptRoot();
    }
    QFileInfo info(QString::fromStdString(path));
    if (info.isRelative() && !root.empty()) info = QFileInfo(QDir(QString::fromStdString(root)), QString::fromStdString(path));
    return info.exists() && info.isFile() ? info.absoluteFilePath() : QString();
}

PreferencesDialog* MainWindow::openPreferences(bool modal) {
    auto* dialog = new PreferencesDialog(this, m_viewport, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (modal && m_fileDialogHook == nullptr) {
        dialog->exec();
        return nullptr;
    }
    dialog->show();
    return dialog;
}

void MainWindow::addRecentProject(const QString& root) {
    if (root.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(root).absoluteFilePath();
    m_recentProjects.removeAll(absolute);
    m_recentProjects.prepend(absolute);
    while (m_recentProjects.size() > kMaxRecentFiles) {
        m_recentProjects.removeLast();
    }
    if (m_options.persistRecentFiles) {
        QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor"))
            .setValue(QStringLiteral("editor/recentProjects"), m_recentProjects);
    }
    rebuildRecentProjectsMenu();
}

void MainWindow::rebuildRecentProjectsMenu() {
    if (m_recentProjectsMenu == nullptr) {
        return;
    }
    m_recentProjectsMenu->clear();
    int index = 1;
    for (const QString& root : m_recentProjects) {
        QAction* action = m_recentProjectsMenu->addAction(
            QStringLiteral("&%1 %2  (%3)").arg(index++).arg(QFileInfo(root).fileName(), QDir::toNativeSeparators(root)));
        action->setData(root);
        const bool exists = QFileInfo(root + QStringLiteral("/project.json")).isFile();
        action->setEnabled(exists);
        connect(action, &QAction::triggered, this, [this, root]() {
            if (maybeSaveChanges()) onProjectOpenRequested(root);
        });
    }
    if (!m_recentProjects.isEmpty()) {
        m_recentProjectsMenu->addSeparator();
        QAction* clear = m_recentProjectsMenu->addAction(tr("Clear List"));
        clear->setObjectName(QStringLiteral("fuseActionClearRecentProjects"));
        connect(clear, &QAction::triggered, this, [this]() {
            m_recentProjects.clear();
            if (m_options.persistRecentFiles) {
                QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor")).remove(QStringLiteral("editor/recentProjects"));
            }
            QTimer::singleShot(0, this, [this]() { rebuildRecentProjectsMenu(); });
        });
    }
    m_recentProjectsMenu->setEnabled(!m_recentProjects.isEmpty());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSaveChanges()) {
        if (m_options.persistLayout) {
            saveSettingsLayout();
        }
        event->accept();
    } else {
        event->ignore();
    }
}

// ---- unified undo (UndoStack + CommandStack) --------------------------------------------------------

void MainWindow::syncUndoJournal_() {
    // Record which stack gained / lost steps since the last look, so Edit > Undo can walk both
    // histories in the order the edits happened. Called under the scene mutex.
    const auto reconcile = [this](UndoSource source, u32 now, u32& seen, bool redoEmpty) {
        if (now > seen) {
            m_undoJournal.insert(m_undoJournal.end(), now - seen, source);
        } else {
            for (u32 drop = seen - now; drop > 0u;) {
                auto it = std::find(m_undoJournal.rbegin(), m_undoJournal.rend(), source);
                if (it == m_undoJournal.rend()) {
                    break;
                }
                m_undoJournal.erase(std::next(it).base());
                --drop;
            }
        }
        if (redoEmpty) { // a new edit dropped this stack's redo branch
            m_redoJournal.erase(std::remove(m_redoJournal.begin(), m_redoJournal.end(), source), m_redoJournal.end());
        }
        seen = now;
    };
    reconcile(UndoSource::Scene, m_host.undoStack().undoCount(), m_seenSceneUndo, !m_host.undoStack().canRedo());
    reconcile(UndoSource::Property, m_host.commandStack().undoDepth(), m_seenPropertyUndo,
              !m_host.commandStack().canRedo());
}

void MainWindow::undoLatest() {
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        syncUndoJournal_();
        if (m_undoJournal.empty() && (m_host.undoStack().canUndo() || m_host.commandStack().canUndo())) {
            // History older than the journal (e.g. evicted entries): fall back to either stack.
            m_undoJournal.push_back(m_host.undoStack().canUndo() ? UndoSource::Scene : UndoSource::Property);
        }
        if (!m_undoJournal.empty()) {
            const UndoSource source = m_undoJournal.back();
            m_undoJournal.pop_back();
            m_redoJournal.push_back(source);
            if (source == UndoSource::Scene) {
                m_host.undoStack().undo();
                m_seenSceneUndo = m_host.undoStack().undoCount();
            } else {
                m_host.undoPropertyEdit();
                m_seenPropertyUndo = m_host.commandStack().undoDepth();
            }
        }
    }
    refreshPanels();
    updateWindowTitle();
}

void MainWindow::redoLatest() {
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        syncUndoJournal_();
        if (!m_redoJournal.empty()) {
            const UndoSource source = m_redoJournal.back();
            m_redoJournal.pop_back();
            m_undoJournal.push_back(source);
            if (source == UndoSource::Scene) {
                m_host.undoStack().redo();
                m_seenSceneUndo = m_host.undoStack().undoCount();
            } else {
                m_host.redoPropertyEdit();
                m_seenPropertyUndo = m_host.commandStack().undoDepth();
            }
        }
    }
    refreshPanels();
    updateWindowTitle();
}

} // namespace fuse::editor::qt

namespace fuse::editor::qt {

bool MainWindow::selectForScreenshot(const QString& spec) {
    if (spec.compare("terrain", Qt::CaseInsensitive) == 0) {
        // --spawn terrain: a Terrain at the origin with a raise stroke and a lowered pit, selected.
        TerrainPanel* panel = terrain();
        if (panel == nullptr) return false;
        const ecs::EntityID e = panel->createTerrain();
        if (!e.valid()) return false;
        panel->setMode(ecs::TerrainStamp::Raise);
        panel->setBrush(7.0, 4.0);
        panel->beginStroke(e, -14.f, -6.f);
        for (int i = 1; i <= 14; ++i) panel->strokeTo(-14.f + 2.f * static_cast<float>(i), -6.f + 0.6f * static_cast<float>(i));
        panel->endStroke();
        panel->setMode(ecs::TerrainStamp::Lower);
        panel->setBrush(6.0, 3.0);
        panel->sculptLocal(e, 10.f, 10.f);
        if (m_viewport != nullptr) {
            m_viewport->panel().camera().pitch = -30.f; // look down onto the terrain
            (void)m_viewport->frameSelection();
            // Show the sculpt brush outline at the viewport centre (as if hovering).
            panel->setSculptInViewport(true);
            m_viewport->updateTerrainBrush(QPointF(m_viewport->width() * 0.5, m_viewport->height() * 0.6));
        }
        return true;
    }
    const bool cube = spec.compare("cube", Qt::CaseInsensitive) == 0;
    if (cube || spec.compare("new", Qt::CaseInsensitive) == 0) {
        QAction* create = findChild<QAction*>(cube ? "fuseActionCreateCube" : "fuseActionCreateSdfSphere");
        if (create == nullptr) {
            return false;
        }
        create->trigger();
        if (m_gameThread.isRunning()) {
            // The game thread owns rendering (HybridComposer::render is render-thread only): wait for
            // it to drain the create command instead of ticking on the UI thread.
            const u32 target = m_host.gameTickCount() + 2u;
            QElapsedTimer waited;
            waited.start();
            while (m_host.gameTickCount() < target && waited.elapsed() < 10000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
        } else {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.gameTick();
        }
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        const ecs::EntityID e = m_host.editorState().primarySelection;
        ecs::Transform* t = m_host.editorScene().registry().get<ecs::Transform>(e);
        if (t == nullptr) {
            return false;
        }
        const ecs::vec3 p = m_viewport->panel().position(), f = m_viewport->panel().forward();
        t->position = ecs::vec3{p.x + f.x * 6.f, p.y + f.y * 6.f, p.z + f.z * 6.f, 1.f};
        t->dirty = true; // world matrix (and the GPU instance) follow the new position
    } else {
        bool ok = false;
        const u32 index = spec.toUInt(&ok);
        if (!ok) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        auto& reg = m_host.editorScene().registry();
        ecs::EntityID found = ecs::EntityID::null();
        reg.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) {
            if (id.index == index) {
                found = id;
            }
        });
        if (found == ecs::EntityID::null()) {
            return false;
        }
        auto& state = m_host.editorState();
        state.selectedEntities = {found};
        state.primarySelection = found;
    }
    refreshPanels();
    m_viewport->refreshGizmo();
    m_viewport->update();
    return true;
}

} // namespace fuse::editor::qt

namespace fuse::editor::qt {

bool MainWindow::showProjectSettings() {
    if (m_projectRoot.isEmpty()) {
        QMessageBox::information(this, tr("Project Settings"), tr("Open a project first."));
        return false;
    }
    ProjectSettingsDialog dialog(m_projectRoot, this);
    if (!dialog.loaded()) {
        QMessageBox::warning(this, tr("Project Settings"), tr("Cannot read project.json in %1").arg(m_projectRoot));
        return false;
    }
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    QString error;
    if (!dialog.save(&error)) {
        QMessageBox::warning(this, tr("Project Settings"), error);
        return false;
    }
    if (m_console != nullptr) {
        m_console->panel().addLog(fuse::log::Level::Info, tr("Project settings saved; reopening %1").arg(m_projectRoot).toUtf8().constData());
        m_console->rerender();
    }
    if (maybeSaveChanges()) {
        openProject(m_projectRoot);
    }
    return true;
}

bool MainWindow::importAssets(const QStringList& files) {
    if (m_projectRoot.isEmpty()) {
        return false;
    }
    QString error;
    const QStringList written = importAssetsIntoProject(m_projectRoot, files, &error);
    if (m_console != nullptr) {
        for (const QString& w : written) m_console->panel().addLog(fuse::log::Level::Info, tr("Imported %1").arg(w).toUtf8().constData());
        if (!error.isEmpty()) m_console->panel().addLog(fuse::log::Level::Warn, tr("Import: %1").arg(error).toUtf8().constData());
        m_console->rerender();
    }
    // Cook on import (async: the UI stays live; the asset browser rescans as each cook lands).
    const QString cookTool = findCookTool();
    for (const QString& w : written) {
        AssetCookJob job;
        if (!makeAssetCookJob(m_projectRoot, w, cookTool, job)) continue;
        auto* process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        ++m_pendingCooks;
        connect(process, &QProcess::finished, this, [this, process, job](int code, QProcess::ExitStatus status) {
            --m_pendingCooks;
            const bool ok = status == QProcess::NormalExit && code == 0;
            if (m_console != nullptr) {
                const QString msg = ok ? tr("Cooked %1 -> %2").arg(job.source, job.output)
                                       : tr("Cook failed for %1: %2").arg(job.source, QString::fromUtf8(process->readAll()).trimmed());
                m_console->panel().addLog(ok ? fuse::log::Level::Info : fuse::log::Level::Warn, msg.toUtf8().constData());
                m_console->rerender();
            }
            ok ? ++m_cookedAssets : ++m_failedCooks;
            m_assetBrowser->rescan();
            process->deleteLater();
        });
        process->start(job.program, job.arguments);
    }
    if (cookTool.isEmpty() && m_console != nullptr && !written.isEmpty()) {
        m_console->panel().addLog(fuse::log::Level::Warn, "Import: fuse_cook not found (set FUSE_COOK); assets copied uncooked");
    }
    m_assetBrowser->rescan();
    return !written.isEmpty() && error.isEmpty();
}

} // namespace fuse::editor::qt
