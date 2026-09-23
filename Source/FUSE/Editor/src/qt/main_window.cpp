#include "main_window.hpp"

#include "editor_panels.hpp"
#include "project_hub_widget.hpp"
#include "property_pane_widget.hpp"
#include "seq_preview_pane_widget.hpp"
#include "viewport_placeholder_widget.hpp"

#include <fuse/editor/command_queue.hpp>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QStyle>
#include <QStatusBar>
#include <QVariant>

namespace fuse::editor::qt {

MainWindow::MainWindow(const QString& samplesRoot, QWidget* parent) : MainWindow(samplesRoot, Options{}, parent) {}

MainWindow::MainWindow(const QString& samplesRoot, const Options& options, QWidget* parent)
    : QMainWindow(parent), m_gameThread(&m_host, &m_sceneMutex, this), m_options(options),
      m_samplesRoot(samplesRoot) {
    setObjectName(QStringLiteral("fuseEditorMainWindow"));
    setWindowTitle(tr("FUSE Editor"));
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
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::onFocusChanged);

    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatusBar);
    m_statusTimer.start();

    if (m_options.startGameLoop) {
        m_gameThread.start();
    }
    if (m_options.startFramePump) {
        m_viewport->setFramePumpEnabled(true);
    }
    refreshPanels();
    refreshStatusBar();
}

MainWindow::~MainWindow() {
    disconnect(qApp, &QApplication::focusChanged, this, &MainWindow::onFocusChanged);
    m_statusTimer.stop();
    m_viewport->setFramePumpEnabled(false);
    m_gameThread.quit();
    m_gameThread.wait();
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
    m_console = new ConsoleWidget();
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
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* quit = file->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = edit->addAction(tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, this, [this]() {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.undoStack().undo();
        }
        refreshPanels();
    });
    QAction* redo = edit->addAction(tr("&Redo"));
    redo->setShortcut(QKeySequence::Redo);
    connect(redo, &QAction::triggered, this, [this]() {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.undoStack().redo();
        }
        refreshPanels();
    });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : m_docks) {
        view->addAction(dock->toggleViewAction());
    }
    view->addSeparator();
    QAction* reset = view->addAction(tr("Reset &Layout"));
    connect(reset, &QAction::triggered, this, &MainWindow::resetToDefaultLayout);

    QMenu* play = menuBar()->addMenu(tr("&Play"));
    QAction* start = play->addAction(tr("&Play"));
    start->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(start, &QAction::triggered, this, [this]() { m_featureBridge.postPlayRequested(); });
    QAction* stop = play->addAction(tr("&Stop"));
    connect(stop, &QAction::triggered, this, [this]() { m_featureBridge.postStopRequested(); });
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
    m_viewport->setProjectLabel(info.fileName());
    m_assetBrowser->setProjectRoot(projectDirectory);

    EditorCommand cmd;
    cmd.kind = CommandKind::SetProperty;
    cmd.propertyName = "project";
    cmd.propertyValue = info.fileName().toStdString();
    m_host.postFromUi(std::move(cmd));
}

void MainWindow::refreshPanels() {
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
    m_console->drainLog();
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_inspector->propertyPane()->refresh();
    }
    m_profiler->refresh();
    m_statusLabel->setText(
        tr("Game ticks: %1 | Pending: %2 | Applied: %3 | PIE: %4")
            .arg(m_host.gameTickCount())
            .arg(m_host.commandQueue().pendingCount())
            .arg(m_host.commandQueue().appliedCount())
            .arg(m_host.editorState().playing ? tr("on") : tr("off")));
}

} // namespace fuse::editor::qt
