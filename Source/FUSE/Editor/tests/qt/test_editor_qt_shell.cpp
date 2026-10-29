// GUI-1 Qt shell gate (offscreen QPA): the desktop editor window a user launches.
//
//   docks      the main window builds with every panel dock (hierarchy, inspector, console, project
//              hub, asset browser, material editor, SDF sculpt, profiler, sequencer) and a viewport
//   toolbar    File / Edit toolbar carries New / Open / Save / Undo / Redo; Undo / Redo enable state
//              follows the host stacks
//   undo       an inspector edit lands through the command system and toolbar Undo / Redo revert /
//              reapply it
//   play       toolbar Play / Stop toggle PIE and the transport enable state
//   assets     asset browser folder navigation (Up / double-click) and scene double-click opens it
//   profiler   frame-time graph, min / avg / max summary, Pause and Clear
//   project    openProject() on a sample loads its world into the hierarchy; the window grabs to a
//              non-empty image (the --screenshot path)
#include "editor_panels.hpp"
#include "main_window.hpp"

#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QToolButton>
#include <QPixmap>
#include <QToolBar>
#include <QTreeWidget>

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

namespace ecs = fuse::ecs;
using fuse::editor::CommandKind;
using fuse::editor::qt::MainWindow;

void tick(MainWindow& window, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().gameTick();
    }
    window.refreshPanels();
    window.updateTransportActions();
    window.updateEditActions();
    QCoreApplication::processEvents();
}

QAction* findAction(MainWindow& window, const char* objectName) {
    return window.findChild<QAction*>(QString::fromLatin1(objectName));
}

int gateDocks(MainWindow& window) {
    const char* names[] = {MainWindow::kHierarchyDock,      MainWindow::kInspectorDock,  MainWindow::kConsoleDock,
                           MainWindow::kProjectHubDock,     MainWindow::kAssetBrowserDock,
                           MainWindow::kMaterialEditorDock, MainWindow::kSdfSculptDock, MainWindow::kProfilerDock,
                           MainWindow::kSequencerDock};
    int present = 0;
    for (const char* name : names) {
        QDockWidget* d = window.dock(name);
        if (d != nullptr && d->widget() != nullptr) {
            ++present;
        } else {
            std::printf("     missing dock %s\n", name);
        }
    }
    expect(present == 9, "docks: all 9 panel docks present with content");
    expect(window.viewport() != nullptr && window.centralWidget() != nullptr, "docks: viewport is the central widget");
    expect(window.hierarchy() && window.inspector() && window.console() && window.profiler() && window.projectHub(),
           "docks: panel accessors bound");
    return 0;
}

int gateToolbar(MainWindow& window) {
    QToolBar* bar = window.mainToolBar();
    expect(bar != nullptr, "toolbar: main toolbar exists");
    if (bar == nullptr) {
        return 1;
    }
    const char* names[] = {MainWindow::kActionNewScene, MainWindow::kActionOpenScene, MainWindow::kActionSave,
                           MainWindow::kActionUndo, MainWindow::kActionRedo};
    bool all = true;
    for (const char* name : names) {
        QAction* a = findAction(window, name);
        all = all && a != nullptr && bar->actions().contains(a) && !a->icon().isNull();
    }
    expect(all, "toolbar: New / Open / Save / Undo / Redo with icons");
    expect(findAction(window, MainWindow::kActionOpenProject) != nullptr, "toolbar: File > Open Project action");
    tick(window);
    expect(!findAction(window, MainWindow::kActionUndo)->isEnabled() &&
               !findAction(window, MainWindow::kActionRedo)->isEnabled(),
           "toolbar: Undo / Redo disabled on a fresh scene");
    return 0;
}

int gateUndo(MainWindow& window) {
    ecs::EntityID e;
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        ecs::Registry& registry = window.host().editorScene().registry();
        e = registry.create();
        ecs::Transform t{};
        t.position = {1.f, 2.f, 3.f, 1.f};
        registry.add(e, t);
        auto& state = window.host().editorState();
        state.primarySelection = e;
        state.selectedEntities = {e};
    }
    tick(window);
    QDoubleSpinBox* x = window.inspector()->fieldSpin(QStringLiteral("transform.position"), 0);
    expect(x != nullptr, "undo: inspector shows the position editor");
    if (x == nullptr) {
        return 1;
    }
    x->setValue(7.5);
    tick(window, 2);
    const auto posX = [&]() {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        const ecs::Transform* t = window.host().editorScene().registry().get<ecs::Transform>(e);
        return t != nullptr ? t->position.x : -999.f;
    };
    expect(std::fabs(posX() - 7.5f) < 1e-4f, "undo: inspector edit applied");
    QAction* undo = findAction(window, MainWindow::kActionUndo);
    QAction* redo = findAction(window, MainWindow::kActionRedo);
    expect(undo->isEnabled(), "undo: Undo enabled after the edit");
    undo->trigger();
    tick(window, 2);
    expect(std::fabs(posX() - 1.f) < 1e-4f, "undo: toolbar Undo restores the old value");
    expect(redo->isEnabled(), "undo: Redo enabled after undo");
    redo->trigger();
    tick(window, 2);
    expect(std::fabs(posX() - 7.5f) < 1e-4f, "undo: toolbar Redo reapplies the edit");
    return 0;
}

int gatePlay(MainWindow& window) {
    QAction* play = window.transportAction(CommandKind::StartPlay);
    QAction* stop = window.transportAction(CommandKind::StopPlay);
    tick(window);
    expect(play->isEnabled() && !stop->isEnabled(), "play: stopped state");
    play->trigger();
    tick(window, 3);
    expect(window.host().editorState().playing && !play->isEnabled() && stop->isEnabled(), "play: Play enters PIE");
    stop->trigger();
    tick(window, 2);
    expect(!window.host().editorState().playing && play->isEnabled() && !stop->isEnabled(), "play: Stop leaves PIE");
    return 0;
}

int gateProject(MainWindow& window) {
    const QString project = QDir(QStringLiteral(FUSE_TEST_SOURCE_DIR)).absoluteFilePath(
        QStringLiteral("Samples/unification/demo_3d_empty"));
    window.openProject(project);
    bool loaded = false;
    for (int i = 0; i < 200 && !loaded; ++i) {
        tick(window);
        loaded = window.syncProjectWorld() || window.hierarchy()->findChild<QTreeWidget*>()->topLevelItemCount() > 0;
    }
    tick(window);
    QTreeWidget* tree = window.hierarchy()->findChild<QTreeWidget*>();
    expect(tree != nullptr && tree->topLevelItemCount() > 0, "project: sample world populates the hierarchy");
    expect(window.windowTitle().contains(QStringLiteral("demo_3d_empty")), "project: window title names the project");
    const QImage shot = window.grab().toImage();
    expect(!shot.isNull() && shot.width() > 200 && shot.height() > 200, "project: window grabs to an image");
    return 0;
}

int gateAssets(MainWindow& window) {
    auto* assets = window.findChild<fuse::editor::qt::AssetBrowserWidget*>();
    expect(assets != nullptr, "assets: asset browser widget");
    if (assets == nullptr) {
        return 1;
    }
    const QString project = QDir(QStringLiteral(FUSE_TEST_SOURCE_DIR)).absoluteFilePath(
        QStringLiteral("Samples/unification/demo_3d_empty"));
    assets->setProjectRoot(project);
    expect(!assets->upButton()->isEnabled() && assets->list()->count() >= 2, "assets: project root listed, Up disabled");
    using R = fuse::editor::AssetBrowser::ActivateResult;
    expect(assets->activatePath(QStringLiteral("worlds")) == R::EnteredFolder && assets->upButton()->isEnabled() &&
               assets->pathLabel()->text() == QStringLiteral("/worlds"),
           "assets: double-click a folder enters it");
    QString requested;
    QObject::connect(assets, &fuse::editor::qt::AssetBrowserWidget::sceneOpenRequested,
                     [&](const QString& path) { requested = path; });
    window.setDirtyPromptHook([](const QString&) { return QMessageBox::Discard; });
    expect(assets->activatePath(QStringLiteral("worlds/example.fuselevel")) == R::OpenScene &&
               requested.endsWith(QStringLiteral("worlds/example.fuselevel")),
           "assets: double-click a scene requests open");
    tick(window, 2);
    expect(window.sceneDisplayName().contains(QStringLiteral("example")), "assets: the scene opens in the editor");
    expect(assets->navigateUp() && !assets->upButton()->isEnabled(), "assets: Up returns to the root");
    return 0;
}

int gateProfiler(MainWindow& window) {
    fuse::editor::qt::ProfilerWidget* prof = window.profiler();
    prof->panel().clearHistory();
    for (int i = 0; i < 40; ++i) {
        fuse::editor::ProfilerPanel::FrameProfileData f{};
        f.cpuMs = (i % 10 == 0) ? 25.f : 8.f;
        prof->pushFrame(f);
    }
    prof->refresh();
    expect(prof->graph() != nullptr && prof->summary()->text().contains(QStringLiteral("max 25.00")),
           "profiler: summary shows min / avg / max");
    expect(prof->panel().cpuStats().overBudget == 4u, "profiler: over-budget frames counted");
    const QImage img = prof->graph()->grab().toImage();
    expect(!img.isNull(), "profiler: graph paints");
    prof->pauseCheck()->setChecked(true);
    fuse::editor::ProfilerPanel::FrameProfileData f{};
    prof->pushFrame(f);
    expect(prof->panel().frameCount() == 40u, "profiler: Pause stops capture");
    prof->pauseCheck()->setChecked(false);
    prof->clearButton()->click();
    expect(prof->panel().frameCount() == 0u, "profiler: Clear empties the history");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    fuse::core::initialize();
    {
        int qtArgc = 1;
        QApplication app(qtArgc, argv);
        MainWindow::Options options;
        options.startGameLoop = false;
        options.startFramePump = false;
        options.embeddedVulkanViewport = false;
        options.persistRecentFiles = false;
        MainWindow window(QString(), options);
        window.show();
        QCoreApplication::processEvents();
        const std::string gate = argc > 1 ? argv[1] : "all";
        if (gate == "docks" || gate == "all") {
            gateDocks(window);
        }
        if (gate == "toolbar" || gate == "all") {
            gateToolbar(window);
        }
        if (gate == "undo" || gate == "all") {
            gateUndo(window);
        }
        if (gate == "play" || gate == "all") {
            gatePlay(window);
        }
        if (gate == "assets" || gate == "all") {
            gateAssets(window);
        }
        if (gate == "profiler" || gate == "all") {
            gateProfiler(window);
        }
        if (gate == "project" || gate == "all") {
            gateProject(window);
        }
    }
    fuse::core::shutdown();
    std::printf("qt shell: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
