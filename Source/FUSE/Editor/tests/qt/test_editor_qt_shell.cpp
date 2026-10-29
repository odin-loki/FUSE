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
//   entity     Entity menu create / duplicate / delete (undoable through the UndoStack)
//   gizmo      transform toolbar (Select / Move / Rotate / Scale, Local, Snap) and viewport handle
//              drags: one drag = one undo step, snap rounds, rotate / scale edit the transform
//   material   Material Editor dock: catalog list, follow-selection, roughness / metallic / colour
//              edits and New / Assign are undoable; the catalog saves / reloads beside the scene
//   sculpt     SDF Sculpt dock: brush controls, Sculpt-in-viewport drags add primitives (one drag =
//              one undo step) and Paint assigns the brush material
//   overlay    gizmo overlay used over the embedded Vulkan window: native child shaped to the handles,
//              mouse input on it drives the same handle drag; --select picks an entity
//   timeline   Timeline dock: key the selection, scrub / play interpolates the transform, drag-retime
//              and delete keys (each one undo step)
//   layout     dock layout + geometry save / restore through QSettings (temp settings path)
//   profiler   frame-time graph, min / avg / max summary, Pause and Clear
//   project    openProject() on a sample loads its world into the hierarchy; the window grabs to a
//              non-empty image (the --screenshot path)
#include "editor_panels.hpp"
#include "main_window.hpp"
#include "seq_preview_pane_widget.hpp"
#include "timeline_widget.hpp"
#include "viewport_placeholder_widget.hpp"

#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/editor/material_catalog.hpp>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPushButton>

#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QImage>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
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
                           MainWindow::kSequencerDock,      MainWindow::kTimelineDock};
    int present = 0;
    for (const char* name : names) {
        QDockWidget* d = window.dock(name);
        if (d != nullptr && d->widget() != nullptr) {
            ++present;
        } else {
            std::printf("     missing dock %s\n", name);
        }
    }
    expect(present == 10, "docks: all 10 panel docks present with content");
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

int gateEntity(MainWindow& window) {
    using fuse::editor::ContextMenuAction;
    QMenu* menu = window.entityMenu();
    expect(menu != nullptr && menu->actions().size() >= 9, "entity: Entity menu with create / duplicate / delete");
    tick(window);
    const int rows0 = window.hierarchy()->entityRowCount();
    QAction* create = findAction(window, "fuseActionCreateSdfSphere");
    expect(create != nullptr, "entity: Create SDF Sphere action");
    if (create == nullptr) {
        return 1;
    }
    create->trigger();
    tick(window);
    const ecs::EntityID created = window.host().editorState().primarySelection;
    expect(window.hierarchy()->entityRowCount() == rows0 + 1 && created != ecs::EntityID::null(),
           "entity: create adds a selected entity");
    expect(window.runEntityAction(ContextMenuAction::Duplicate), "entity: duplicate the selection");
    tick(window);
    expect(window.hierarchy()->entityRowCount() == rows0 + 2, "entity: duplicate adds a row");
    findAction(window, "fuseActionDelete")->trigger();
    tick(window);
    expect(window.hierarchy()->entityRowCount() == rows0 + 1, "entity: delete removes the selection");
    window.updateEditActions();
    findAction(window, MainWindow::kActionUndo)->trigger();
    tick(window);
    expect(window.hierarchy()->entityRowCount() == rows0 + 2, "entity: undo restores the deleted entity");
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().editorState().primarySelection = ecs::EntityID::null();
        window.host().editorState().selectedEntities.clear();
    }
    expect(!window.runEntityAction(ContextMenuAction::Delete), "entity: delete without a selection is a no-op");
    return 0;
}

// Drag `axis` of the current gizmo from its handle midpoint by `dx` / `dy` logical pixels.
bool dragHandle(MainWindow& window, int axis, double dx, double dy) {
    auto* vp = window.viewport();
    if (!vp->refreshGizmo()) {
        return false;
    }
    const auto& h = vp->gizmo().handles()[axis];
    const qreal inv = 1.0 / vp->devicePixelRatioF();
    const QPointF start((h.x0 + h.x1) * 0.5 * inv, (h.y0 + h.y1) * 0.5 * inv);
    if (!vp->gizmoPress(start)) {
        return false;
    }
    for (int i = 1; i <= 4; ++i) {
        vp->gizmoMove(start + QPointF(dx * i / 4.0, dy * i / 4.0));
    }
    vp->gizmoRelease();
    return true;
}

ecs::Transform selectedTransform(MainWindow& window) {
    std::lock_guard<std::mutex> lock(window.sceneMutex());
    const ecs::EntityID e = window.host().editorState().primarySelection;
    const ecs::Transform* t = window.host().editorScene().registry().get<ecs::Transform>(e);
    return t != nullptr ? *t : ecs::Transform{};
}

int gateOverlay(MainWindow& window) {
    auto* vp = window.viewport();
    findAction(window, "fuseActionToolTranslate")->trigger();
    expect(window.selectForScreenshot("new"), "overlay: --select new spawns + selects a sphere in front");
    vp->setOverlayForced(true);
    QCoreApplication::processEvents();
    expect(vp->overlayActive() && vp->overlayWidget() != nullptr, "overlay: native overlay shown for the gizmo");
    QWidget* ov = vp->overlayWidget();
    expect(ov != nullptr && ov->testAttribute(Qt::WA_NativeWindow), "overlay: overlay is a native child window");
    const auto& h = vp->gizmo().handles()[0];
    const qreal inv = 1.0 / vp->devicePixelRatioF();
    const QPoint mid(static_cast<int>((h.x0 + h.x1) * 0.5 * inv), static_cast<int>((h.y0 + h.y1) * 0.5 * inv));
    expect(ov != nullptr && !ov->mask().isEmpty() && ov->mask().contains(mid), "overlay: mask covers the X handle");
    expect(ov != nullptr && !ov->mask().contains(QPoint(2, 2)), "overlay: mask leaves the rest of the viewport clear");
    const QImage& img = vp->overlayImage();
    expect(!img.isNull() && qAlpha(img.pixel(static_cast<int>(mid.x() * vp->devicePixelRatioF()),
                                              static_cast<int>(mid.y() * vp->devicePixelRatioF()))) > 0,
           "overlay: handle pixels drawn");

    // A drag delivered to the overlay window moves the entity (same path as the viewport).
    const ecs::Transform t0 = selectedTransform(window);
    const std::size_t undo0 = window.host().undoStack().undoCount();
    const QPointF dir = QPointF(h.x1 - h.x0, h.y1 - h.y0) / std::hypot(h.x1 - h.x0, h.y1 - h.y0);
    auto send = [&](QEvent::Type type, const QPointF& p, Qt::MouseButton b, Qt::MouseButtons bs) {
        QMouseEvent ev(type, p, ov->mapToGlobal(p), b, bs, Qt::NoModifier);
        QCoreApplication::sendEvent(ov, &ev);
    };
    send(QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPointF(mid) + dir * 20.0, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPointF(mid) + dir * 40.0, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPointF(mid) + dir * 40.0, Qt::LeftButton, Qt::NoButton);
    const ecs::Transform t1 = selectedTransform(window);
    expect(t1.position.x > t0.position.x + 1e-3f, "overlay: drag on the overlay moves the entity along X");
    expect(window.host().undoStack().undoCount() == undo0 + 1, "overlay: overlay drag is one undo step");
    vp->setTransformTool(fuse::editor::TransformTool::Select);
    vp->syncOverlay(true);
    expect(!vp->overlayActive(), "overlay: hidden when nothing is drawn (Select tool)");
    vp->setOverlayForced(false);
    findAction(window, "fuseActionToolTranslate")->trigger();
    return 0;
}

int gateTimeline(MainWindow& window) {
    fuse::editor::qt::TimelineWidget* tw = window.timeline();
    expect(tw != nullptr && window.dock(MainWindow::kTimelineDock) != nullptr, "timeline: Timeline dock present");
    if (tw == nullptr) {
        return 0;
    }
    findAction(window, "fuseActionCreateSdfSphere")->trigger();
    tick(window);
    tw->scrubTo(0);
    expect(tw->keySelection(), "timeline: key the selection at 0 ms");
    const std::size_t undo0 = window.host().undoStack().undoCount();
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& reg = window.host().editorScene().registry();
        ecs::Transform* t = reg.get<ecs::Transform>(window.host().editorState().primarySelection);
        t->position.x = 0.f;
    }
    tw->scrubTo(2000);
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& reg = window.host().editorScene().registry();
        ecs::Transform* t = reg.get<ecs::Transform>(window.host().editorState().primarySelection);
        t->position.x = 8.f;
    }
    expect(tw->keySelection(), "timeline: key again at 2000 ms");
    expect(window.host().undoStack().undoCount() == undo0 + 1, "timeline: a key is one undo step");
    expect(tw->model().keyCount() == 18u, "timeline: 9 channel lanes x 2 keys");
    tw->scrubTo(500);
    expect(std::fabs(selectedTransform(window).position.x - 2.f) < 1e-3f, "timeline: scrub interpolates position.x");
    const int lane = tw->model().findTrack(window.host().editorState().primarySelection.index, "position.x");
    expect(tw->selectKey(lane, 1), "timeline: select the 2000 ms key");
    expect(tw->moveSelectedKey(4000) && tw->model().tracks()[static_cast<size_t>(lane)].keyframes()[1].time_ms == 4000,
           "timeline: retime the key to 4000 ms");
    tw->scrubTo(1000);
    expect(std::fabs(selectedTransform(window).position.x - 2.f) < 1e-3f, "timeline: retimed curve (x=2 at 1 s)");
    window.undoLatest();
    expect(tw->model().tracks()[static_cast<size_t>(lane)].keyframes()[1].time_ms == 2000, "timeline: undo the retime");
    tw->selectKey(lane, 0);
    expect(tw->deleteSelectedKey() && tw->model().keyCount() == 17u, "timeline: delete a key");
    window.undoLatest();
    expect(tw->model().keyCount() == 18u, "timeline: undo the delete");
    // Curves: cubic (flat) on the first key eases position.x; the curve strip draws; one undo step.
    expect(tw->selectKey(lane, 0), "timeline: select the 0 ms key");
    const std::size_t undoC = window.host().undoStack().undoCount();
    expect(tw->setSelectedKeyInterp(fuse::cinematics::KeyInterp::Cubic, fuse::cinematics::TangentMode::Flat),
           "timeline: set cubic (flat) on the key");
    expect(window.host().undoStack().undoCount() == undoC + 1, "timeline: interpolation change is one undo step");
    tw->selectKey(lane, 1);
    tw->setSelectedKeyInterp(fuse::cinematics::KeyInterp::Cubic, fuse::cinematics::TangentMode::Flat);
    tw->selectKey(lane, 0);
    tw->scrubTo(200);
    expect(selectedTransform(window).position.x < 0.8f * 0.5f, "timeline: flat cubic eases in");
    expect(std::fabs(tw->sampleSelectedTrack(1000) - 4.f) < 1e-3f, "timeline: cubic midpoint is symmetric");
    expect(tw->setSelectedKeyTangents(0.f, 20.f) &&
               tw->model().tracks()[static_cast<size_t>(lane)].keyframes()[0].tangent_mode ==
                   fuse::cinematics::TangentMode::User,
           "timeline: tangent edit switches to user tangents");
    expect(tw->sampleSelectedTrack(200) > 0.8f, "timeline: steep out tangent leads the linear curve");
    const QImage strip = tw->canvas()->grab().toImage();
    int curvePixels = 0;
    for (int y = tw->canvas()->curveTop(); y < std::min(strip.height(), tw->canvas()->curveTop() + 80); ++y) {
        for (int x = 0; x < strip.width(); ++x) {
            const QColor c = strip.pixelColor(x, y);
            curvePixels += (c.green() > 180 && c.red() < 170) ? 1 : 0;
        }
    }
    expect(curvePixels > 40, "timeline: curve strip plots the selected lane");
    window.undoLatest();
    window.undoLatest();
    window.undoLatest();
    expect(tw->model().tracks()[static_cast<size_t>(lane)].keyframes()[0].interp == fuse::cinematics::KeyInterp::Linear,
           "timeline: undo restores linear");
    // GUI-7 navigation: key jumps, zoom/pan, snap, loop.
    tw->scrubTo(500);
    expect(tw->jumpToKey(1) && tw->model().playheadMs() == 2000, "timeline: ] jumps to the next key");
    expect(tw->jumpToKey(-1) && tw->model().playheadMs() == 0, "timeline: [ jumps to the previous key");
    expect(!tw->jumpToKey(-1), "timeline: no key before the first");
    const int dur = tw->model().durationMs();
    expect(tw->viewStartMs() == 0 && tw->viewSpanMs() == dur, "timeline: view fits the clip");
    tw->zoomBy(4.0, 1000);
    expect(tw->viewSpanMs() == dur / 4 && tw->viewStartMs() <= 1000 && tw->viewStartMs() + tw->viewSpanMs() >= 1000,
           "timeline: zoom keeps the anchor visible");
    const int x1000 = tw->canvas()->msToX(1000);
    tw->panBy(100);
    expect(tw->canvas()->msToX(1000) < x1000, "timeline: pan shifts the lanes");
    tw->zoomBy(1e6, 1000);
    expect(tw->viewSpanMs() >= 50, "timeline: zoom clamps at 50 ms");
    tw->zoomToFit();
    expect(tw->viewSpanMs() == dur, "timeline: Fit restores the whole clip");
    tw->setSnapFps(30);
    expect(tw->snapMs(1010) == 1000 && tw->snapMs(1020) == 1033, "timeline: 30 fps snap");
    tw->setSnapFps(0);
    expect(tw->snapMs(1014) == 1010, "timeline: default 10 ms snap");
    tw->setLooping(true);
    tw->scrubTo(dur - 100);
    tw->setPlaying(true);
    {
        QElapsedTimer lc;
        lc.start();
        int maxSeen = 0;
        bool wrapped = false;
        while (lc.elapsed() < 2000 && !wrapped) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const int ph = tw->model().playheadMs();
            wrapped = ph < maxSeen;
            maxSeen = std::max(maxSeen, ph);
        }
        expect(wrapped && tw->playing(), "timeline: loop wraps and keeps playing");
    }
    tw->setPlaying(false);
    tw->setLooping(false);
    tw->scrubTo(0);
    tw->setPlaying(true);
    QElapsedTimer clock;
    clock.start();
    while (tw->playing() && clock.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    expect(!tw->playing() && tw->model().playheadMs() == tw->model().durationMs(), "timeline: Play runs to the end");
    expect(std::fabs(selectedTransform(window).position.x - 8.f) < 1e-3f, "timeline: end of play lands on the last key");
    // Sequencer <-> Timeline link: one playhead both ways; unlinking stops following.
    fuse::editor::qt::SeqPreviewPaneWidget* seq = window.sequencer();
    expect(seq != nullptr && seq->linked(), "sequencer: linked to the timeline by default");
    if (seq != nullptr) {
        const fuse::u32 posts0 = seq->scrubPostCount();
        tw->scrubTo(1500);
        expect(seq->scrubBarMs() == 1500 && seq->scrubPostCount() > posts0,
               "sequencer: timeline scrub moves the sequencer playhead and samples the sequence");
        seq->scrubBar()->setValue(500);
        expect(tw->model().playheadMs() == 500 && std::fabs(selectedTransform(window).position.x - 2.f) < 1e-3f,
               "sequencer: sequencer scrub drives the timeline (and the keyed transform)");
        seq->setLinked(false);
        tw->scrubTo(1000);
        expect(seq->scrubBarMs() == 500, "sequencer: unlinked sequencer ignores the timeline");
        seq->scrubBar()->setValue(250);
        expect(tw->model().playheadMs() == 1000, "sequencer: unlinked scrub leaves the timeline alone");
        seq->setLinked(true);
    }
    const QPixmap shot = tw->grab();
    expect(!shot.isNull(), "timeline: dock grabs");
    return 0;
}

int gateGizmo(MainWindow& window) {
    using fuse::editor::TransformTool;
    using fuse::editor::TransformSpace;
    QToolBar* bar = window.transformToolBar();
    expect(bar != nullptr && bar->actions().size() >= 6, "gizmo: Transform toolbar with tools, Local and Snap");
    QAction* select = findAction(window, "fuseActionToolSelect");
    QAction* move = findAction(window, "fuseActionToolTranslate");
    QAction* rotate = findAction(window, "fuseActionToolRotate");
    QAction* scale = findAction(window, "fuseActionToolScale");
    QAction* snap = findAction(window, "fuseActionSnap");
    QAction* local = findAction(window, "fuseActionSpaceLocal");
    if (!select || !move || !rotate || !scale || !snap || !local) {
        expect(false, "gizmo: tool actions present");
        return 1;
    }
    expect(move->isChecked(), "gizmo: Move is the default tool");
    auto* vp = window.viewport();
    // Put a fresh entity 6 units in front of the camera.
    findAction(window, "fuseActionCreateSdfSphere")->trigger();
    tick(window);
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        const ecs::EntityID e = window.host().editorState().primarySelection;
        ecs::Transform* t = window.host().editorScene().registry().get<ecs::Transform>(e);
        const ecs::vec3 p = vp->panel().position(), f = vp->panel().forward();
        if (t != nullptr) {
            t->position = ecs::vec3{p.x + f.x * 6.f, p.y + f.y * 6.f, p.z + f.z * 6.f, 1.f};
            t->rotation = ecs::quat{};
            t->scale = ecs::vec3{1.f, 1.f, 1.f, 0.f};
        }
    }
    expect(vp->refreshGizmo(), "gizmo: handles laid out for the selection");
    const ecs::Transform t0 = selectedTransform(window);
    const auto& hx = vp->gizmo().handles()[0];
    const double sx = hx.x1 - hx.x0, sy = hx.y1 - hx.y0, sl = std::hypot(sx, sy);
    const auto undo0 = window.host().undoStack().undoCount();
    expect(dragHandle(window, 0, sx / sl * 45.0, sy / sl * 45.0), "gizmo: drag the X handle");
    tick(window);
    const ecs::Transform t1 = selectedTransform(window);
    expect(t1.position.x > t0.position.x + 0.05f && std::fabs(t1.position.y - t0.position.y) < 1e-4f &&
               std::fabs(t1.position.z - t0.position.z) < 1e-4f,
           "gizmo: X drag moves only along world X");
    expect(window.host().undoStack().undoCount() == undo0 + 1, "gizmo: one drag is one undo step");
    window.updateEditActions();
    findAction(window, MainWindow::kActionUndo)->trigger();
    tick(window);
    expect(std::fabs(selectedTransform(window).position.x - t0.position.x) < 1e-4f, "gizmo: undo restores the position");

    snap->setChecked(true);
    expect(vp->gizmo().settings().snap, "gizmo: Snap toggles snapping");
    dragHandle(window, 0, sx / sl * 37.0, sy / sl * 37.0);
    tick(window);
    const float snapped = selectedTransform(window).position.x - t0.position.x;
    expect(std::fabs(snapped / 0.5f - std::round(snapped / 0.5f)) < 1e-3f && std::fabs(snapped) > 0.f,
           "gizmo: snapped move lands on 0.5 steps");
    snap->setChecked(false);

    rotate->trigger();
    expect(vp->gizmo().settings().tool == TransformTool::Rotate, "gizmo: Rotate tool selected");
    const auto& hy = vp->gizmo().handles()[1];
    const double ry = std::hypot(hy.x1 - hy.x0, hy.y1 - hy.y0);
    // Move perpendicular to the projected Y handle.
    expect(dragHandle(window, 1, -(hy.y1 - hy.y0) / ry * 40.0, (hy.x1 - hy.x0) / ry * 40.0), "gizmo: drag the Y ring");
    tick(window);
    const ecs::quat q = selectedTransform(window).rotation;
    expect(std::fabs(q.y) > 0.05f && std::fabs(q.x) < 1e-4f && std::fabs(q.z) < 1e-4f, "gizmo: rotate spins about Y");

    local->setChecked(true);
    expect(vp->gizmo().settings().space == TransformSpace::Local, "gizmo: Local switches the axis space");
    local->setChecked(false);

    scale->trigger();
    vp->refreshGizmo();
    const auto& hz = vp->gizmo().handles()[2];
    const double zl = std::hypot(hz.x1 - hz.x0, hz.y1 - hz.y0);
    expect(dragHandle(window, 2, (hz.x1 - hz.x0) / zl * 30.0, (hz.y1 - hz.y0) / zl * 30.0), "gizmo: drag the Z box");
    tick(window);
    const ecs::Transform ts = selectedTransform(window);
    expect(ts.scale.z > 1.05f && std::fabs(ts.scale.x - 1.f) < 1e-4f, "gizmo: scale grows only Z");

    QImage shot = vp->grab().toImage();
    expect(!shot.isNull(), "gizmo: viewport grabs with handles drawn");

    select->trigger();
    expect(!vp->refreshGizmo(), "gizmo: Select tool hides the handles");
    move->trigger();
    return 0;
}

int gateMaterial(MainWindow& window) {
    using fuse::editor::AssignMaterialCommand;
    using fuse::u32;
    auto* mat = window.materialEditor();
    expect(mat != nullptr, "material: Material Editor dock widget");
    if (mat == nullptr) {
        return 1;
    }
    auto& catalog = window.host().materials();
    expect(catalog.count() >= 5u && mat->materialList()->count() == static_cast<int>(catalog.count()),
           "material: catalog populated and listed");
    findAction(window, "fuseActionCreateSdfSphere")->trigger();
    tick(window);
    const ecs::EntityID e = window.host().editorState().primarySelection;
    expect(mat->currentMaterial() == 0u, "material: follows the selection (new sphere uses 0)");

    mat->selectMaterial(2);
    expect(mat->assignToSelection(), "material: assign Gold to the selection");
    u32 id = 99;
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        AssignMaterialCommand::materialOf(window.host().editorScene().registry(), e, id);
    }
    expect(id == 2u, "material: SDFObject material_id updated");

    const float r0 = catalog.entry(2).state.roughness;
    expect(mat->setRoughness(0.8), "material: set roughness");
    expect(mat->setMetallic(0.25), "material: set metallic");
    expect(std::fabs(catalog.entry(2).state.roughness - 0.8f) < 1e-5f &&
               std::fabs(catalog.entry(2).state.metallic - 0.25f) < 1e-5f,
           "material: catalog reflects the edits");
    const QImage before = mat->previewImage();
    expect(mat->setBaseColor(QColor(30, 200, 60)), "material: set base colour");
    expect(catalog.entry(2).state.baseColorG > catalog.entry(2).state.baseColorR, "material: colour stored linear");
    expect(!before.isNull() && mat->previewImage() != before, "material: preview re-renders");

    window.updateEditActions();
    findAction(window, MainWindow::kActionUndo)->trigger(); // merged roughness / metallic / colour step
    tick(window);
    expect(std::fabs(catalog.entry(2).state.roughness - r0) < 1e-5f, "material: undo reverts the edit");

    const u32 n0 = catalog.count();
    const u32 created = mat->newMaterial();
    tick(window);
    expect(catalog.count() == n0 + 1u && created == n0 && mat->materialList()->count() == static_cast<int>(n0 + 1u),
           "material: New appends and lists a material");
    findAction(window, MainWindow::kActionUndo)->trigger();
    tick(window);
    expect(catalog.count() == n0 && mat->materialList()->count() == static_cast<int>(n0),
           "material: undo removes the new material");

    QTemporaryDir dir;
    const std::string scene = (dir.path() + QStringLiteral("/mat_scene.fusescene")).toStdString();
    mat->selectMaterial(3);
    mat->setRoughness(0.42);
    // Texture / shader fields: the shader combo and texture path reach the catalog (and RenderMaterial3D).
    expect(mat->setShader(2u) && mat->shaderCombo()->currentIndex() == 2, "material: shader combo -> Metal");
    expect(mat->setTexture(QStringLiteral("textures/brick.png")) && mat->setUvScale(3.0), "material: set texture");
    {
        const fuse::world3d::RenderMaterial3D rm = fuse::editor::MaterialCatalog::toRenderMaterial(catalog.entry(3).state);
        expect(rm.shader == 2u && rm.baseColorTexture == "textures/brick.png" && std::fabs(rm.uvScale - 3.f) < 1e-5f,
               "material: shader + texture reach RenderMaterial3D");
    }
    expect(window.host().saveSceneAs(scene), "material: save scene");
    expect(QFileInfo::exists(QString::fromStdString(fuse::editor::MaterialCatalog::sidecarPath(scene))),
           "material: sidecar written");
    expect(window.host().openScene(scene), "material: reopen scene");
    tick(window);
    expect(std::fabs(catalog.entry(3).state.roughness - 0.42f) < 1e-5f, "material: catalog round-trips");
    expect(catalog.entry(3).state.baseColorTexture == "textures/brick.png" && catalog.entry(3).state.proceduralFnId == 2u,
           "material: texture + shader round-trip");
    return 0;
}

int gateSculpt(MainWindow& window) {
    using fuse::u32;
    auto* sculpt = window.sdfSculpt();
    auto* vp = window.viewport();
    expect(sculpt != nullptr && sculpt->sculptButton() != nullptr, "sculpt: SDF Sculpt dock with a sculpt toggle");
    if (sculpt == nullptr) {
        return 1;
    }
    expect(sculpt->materialCombo()->count() == static_cast<int>(window.host().materials().count()),
           "sculpt: brush material list mirrors the catalog");
    findAction(window, "fuseActionCreateSdfSphere")->trigger();
    tick(window);
    const ecs::EntityID target = window.host().editorState().primarySelection;
    QPointF centre;
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        ecs::Transform* t = window.host().editorScene().registry().get<ecs::Transform>(target);
        const ecs::vec3 p = vp->panel().position(), f = vp->panel().forward();
        t->position = ecs::vec3{p.x + f.x * 6.f, p.y + f.y * 6.f, p.z + f.z * 6.f, 1.f};
        t->dirty = true;
        float px = 0.f, py = 0.f;
        vp->panel().worldToScreen(t->position, px, py);
        centre = QPointF(px / vp->devicePixelRatioF(), py / vp->devicePixelRatioF());
    }
    tick(window);
    sculpt->sculptButton()->setChecked(true);
    expect(vp->sculptMode(), "sculpt: toggle enables viewport sculpt mode");
    const auto count0 = window.host().editorScene().registry().count();
    const auto undo0 = window.host().undoStack().undoCount();
    sculpt->panel().setStrokeSpacing(0.f);
    vp->sculptPress(centre);
    for (int i = 1; i <= 3; ++i) {
        vp->sculptMove(centre + QPointF(6.0 * i, 0.0));
    }
    vp->sculptRelease();
    tick(window);
    const auto count1 = window.host().editorScene().registry().count();
    expect(count1 >= count0 + 2u, "sculpt: an Add drag spawns brush primitives");
    expect(window.host().undoStack().undoCount() == undo0 + 1u, "sculpt: one drag is one undo step");
    window.updateEditActions();
    findAction(window, MainWindow::kActionUndo)->trigger();
    tick(window);
    expect(window.host().editorScene().registry().count() == count0, "sculpt: undo removes the whole stroke");

    sculpt->findChild<QComboBox*>(QStringLiteral("fuseSculptOp"))->setCurrentIndex(4); // Paint
    sculpt->materialCombo()->setCurrentIndex(2);
    vp->sculptPress(centre);
    vp->sculptRelease();
    tick(window);
    u32 id = 0;
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        fuse::editor::AssignMaterialCommand::materialOf(window.host().editorScene().registry(), target, id);
    }
    expect(id == 2u, "sculpt: Paint assigns the brush material");
    QImage shot = vp->grab().toImage();
    expect(!shot.isNull(), "sculpt: viewport grabs with the brush cursor");
    sculpt->sculptButton()->setChecked(false);
    expect(!vp->sculptMode(), "sculpt: toggle off returns to select / gizmo");
    sculpt->findChild<QComboBox*>(QStringLiteral("fuseSculptOp"))->setCurrentIndex(0);
    return 0;
}

int gateLayout(MainWindow& window) {
    QTemporaryDir dir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QDockWidget* profiler = window.dock(MainWindow::kProfilerDock);
    QDockWidget* sculpt = window.dock(MainWindow::kSdfSculptDock);
    profiler->hide();
    sculpt->setFloating(true);
    window.saveSettingsLayout();
    window.resetToDefaultLayout();
    QCoreApplication::processEvents();
    expect(profiler->isVisible() && !sculpt->isFloating(), "layout: reset restores the default docks");
    expect(window.restoreSettingsLayout(), "layout: saved layout restores");
    QCoreApplication::processEvents();
    expect(!profiler->isVisible() && sculpt->isFloating(), "layout: hidden / floating docks come back as saved");
    window.resetToDefaultLayout();
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
        if (gate == "entity" || gate == "all") {
            gateEntity(window);
        }
        if (gate == "gizmo" || gate == "all") {
            gateGizmo(window);
        }
        if (gate == "material" || gate == "all") {
            gateMaterial(window);
        }
        if (gate == "sculpt" || gate == "all") {
            gateSculpt(window);
        }
        if (gate == "overlay" || gate == "all") {
            gateOverlay(window);
        }
        if (gate == "timeline" || gate == "all") {
            gateTimeline(window);
        }
        if (gate == "layout" || gate == "all") {
            gateLayout(window);
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
