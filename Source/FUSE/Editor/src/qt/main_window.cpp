#include "main_window.hpp"

#include "editor_panels.hpp"
#include "project_hub_widget.hpp"
#include "property_pane_widget.hpp"
#include "seq_preview_pane_widget.hpp"
#include "viewport_placeholder_widget.hpp"

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/project/loader.hpp>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
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
    setCentralWidget(m_viewport);

    buildDocks();
    buildTransport();
    buildMenus();
    resetToDefaultLayout();

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel, 1);

    connect(m_viewport, &ViewportPlaceholderWidget::sceneEdited, this, &MainWindow::refreshPanels);
    connect(m_viewport, &ViewportPlaceholderWidget::selectionChanged, this, &MainWindow::refreshPanels);
    connect(m_hierarchy, &HierarchyWidget::sceneEdited, this, &MainWindow::refreshPanels);
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
    refreshPanels();
    refreshStatusBar();
    updateWindowTitle();
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

    addPanelDock(kHierarchyDock, tr("Hierarchy"), m_hierarchy);
    addPanelDock(kProjectHubDock, tr("Projects"), m_projectHub);
    addPanelDock(kAssetBrowserDock, tr("Asset Browser"), m_assetBrowser);
    addPanelDock(kInspectorDock, tr("Inspector"), m_inspector);
    addPanelDock(kMaterialEditorDock, tr("Material Editor"), m_materialEditor);
    addPanelDock(kSdfSculptDock, tr("SDF Sculpt"), m_sdfSculpt);
    addPanelDock(kConsoleDock, tr("Console"), m_console);
    addPanelDock(kProfilerDock, tr("Profiler"), m_profiler);
    addPanelDock(kSequencerDock, tr("Sequencer"), m_sequencer);

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
    file->addSeparator();
    connect(addFileAction(tr("&Save"), kActionSave, QKeySequence::Save), &QAction::triggered, this,
            &MainWindow::onFileSave);
    connect(addFileAction(tr("Save &As..."), kActionSaveAs, QKeySequence::SaveAs), &QAction::triggered, this,
            &MainWindow::onFileSaveAs);
    file->addSeparator();
    connect(addFileAction(tr("New &Project..."), kActionNewProject, QKeySequence()), &QAction::triggered,
            m_projectHub, &ProjectHubWidget::runNewProjectWizard);
    file->addSeparator();
    QAction* quit = addFileAction(tr("&Quit"), kActionQuit, QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = edit->addAction(tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, this, &MainWindow::undoLatest);
    QAction* redo = edit->addAction(tr("&Redo"));
    redo->setShortcut(QKeySequence::Redo);
    connect(redo, &QAction::triggered, this, &MainWindow::redoLatest);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : m_docks) {
        view->addAction(dock->toggleViewAction());
    }
    view->addSeparator();
    QAction* reset = view->addAction(tr("Reset &Layout"));
    connect(reset, &QAction::triggered, this, &MainWindow::resetToDefaultLayout);

    QMenu* play = menuBar()->addMenu(tr("&Play"));
    play->addAction(m_actPlay);
    play->addAction(m_actPause);
    play->addAction(m_actResume);
    play->addAction(m_actStep);
    play->addAction(m_actStop);
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
    m_actStop = makeAction(tr("&Stop"), "fuseActionStop", CommandKind::StopPlay, QStyle::SP_MediaStop);
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

void MainWindow::updateTransportActions() {
    if (m_actPlay == nullptr) {
        return;
    }
    const EditorState& state = m_host.editorState();
    const bool playing = state.playing;
    const bool paused = state.playing && state.paused;
    m_actPlay->setEnabled(!playing);
    m_actPause->setEnabled(playing && !paused);
    m_actResume->setEnabled(paused);
    m_actStep->setEnabled(paused);
    m_actStop->setEnabled(playing);
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

    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    tabifyDockWidget(consoleDock, profilerDock);
    tabifyDockWidget(consoleDock, sequencerDock);

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
    m_projectLabel = info.fileName();
    updateWindowTitle();

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
    syncProjectWorld();
    m_console->drainLog();
    updateTransportActions();
    u32 applied = 0;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        syncUndoJournal_();
        applied = m_host.commandQueue().appliedCount();
    }
    if (applied != m_lastAppliedCommands) {
        // The game tick applied posted commands (inspector edits, add / remove, reparent).
        m_lastAppliedCommands = applied;
        refreshPanels();
    } else {
        m_inspector->refresh(); // cheap when nothing changed; picks up gizmo / PIE edits
    }
    updateWindowTitle();
    m_profiler->refresh();
    m_statusLabel->setText(
        tr("Game ticks: %1 | Pending: %2 | Applied: %3 | PIE: %4")
            .arg(m_host.gameTickCount())
            .arg(m_host.commandQueue().pendingCount())
            .arg(m_host.commandQueue().appliedCount())
            .arg(m_host.editorState().playing ? tr("on") : tr("off")));
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSaveChanges()) {
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
