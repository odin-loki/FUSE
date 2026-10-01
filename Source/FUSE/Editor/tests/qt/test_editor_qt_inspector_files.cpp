// E20 Qt gates (offscreen QPA) on the real FUSE editor host (fuse_editor_qt):
//
//   inspector  MP-B6-QT-INSPECTOR / UNI-U6-INSP-1: typed editors generated for the selection's
//              components (float / vec3 / Euler / colour / enum / bool / asset path); each edit
//              posts SetProperty -> game tick -> registry value; a spin-box drag is one undo step and
//              Edit > Undo restores the value; Add Component menu / Remove buttons post
//              AddComponent / RemoveComponent (undoable, byte-exact restore)
//   reparent   hierarchy internal-move drag-and-drop: a synthetic QDropEvent on a row reparents
//              the dragged entity (ReparentObject + kReparentKeepWorldPose) keeping its world pose;
//              a drop on empty space un-parents; Edit > Undo restores the parent and local TRS
//   file_menu  UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES: File > New -> edit -> Save As -> Open Recent
//              shows the saved entity; the title carries the scene name and '*' while dirty; the
//              unsaved-changes prompt (auto-answered through the test hook) guards New / Open /
//              close, Cancel keeps the scene, Discard drops it, Save saves it
//   wizard     Project hub New Project wizard (dimension + module toggles) writes project.json
//              through the E15 manifest writer; the project loads in the manifest loader and in a
//              fresh EditorHost
#include "editor_panels.hpp"
#include "main_window.hpp"
#include "project_hub_widget.hpp"

#include <fuse/core/init.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/project/loader.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QRadioButton>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
using fuse::editor::qt::HierarchyWidget;
using fuse::editor::qt::InspectorWidget;
using fuse::editor::qt::MainWindow;
using fuse::editor::qt::NewProjectWizard;

void tick(MainWindow& window, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().gameTick();
    }
    window.refreshPanels();
    window.updateWindowTitle();
    QCoreApplication::processEvents();
}

QAction* editAction(MainWindow& window, const QString& text) {
    for (QAction* top : window.menuBar()->actions()) {
        if (top->menu() == nullptr) {
            continue;
        }
        for (QAction* a : top->menu()->actions()) {
            if (a->text() == text) {
                return a;
            }
        }
    }
    return nullptr;
}

ecs::EntityID makeEntity(MainWindow& window, ecs::vec3 position) {
    std::lock_guard<std::mutex> lock(window.sceneMutex());
    ecs::Registry& registry = window.host().editorScene().registry();
    const ecs::EntityID id = registry.create();
    ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    registry.add(id, t);
    return id;
}

void select(MainWindow& window, ecs::EntityID id) {
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& state = window.host().editorState();
        state.primarySelection = id;
        state.selectedEntities = {id};
    }
    window.refreshPanels();
}

template <typename T>
T component(MainWindow& window, ecs::EntityID id) {
    std::lock_guard<std::mutex> lock(window.sceneMutex());
    const T* c = window.host().editorScene().registry().get<T>(id);
    return c != nullptr ? *c : T{};
}

template <typename T>
bool hasComponent(MainWindow& window, ecs::EntityID id) {
    std::lock_guard<std::mutex> lock(window.sceneMutex());
    return window.host().editorScene().registry().has<T>(id);
}

bool closeTo(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// ---- inspector ------------------------------------------------------------------------------------

int gateInspector(MainWindow& window) {
    InspectorWidget* inspector = window.inspector();
    const ecs::EntityID e = makeEntity(window, {1.f, 2.f, 3.f});
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& registry = window.host().editorScene().registry();
        registry.add(e, ecs::RigidBody{});
        registry.add(e, ecs::PointLight{});
    }
    select(window, e);
    const QStringList sections = inspector->componentSections();
    std::printf("inspector sections: %s\n", qPrintable(sections.join(QStringLiteral(", "))));
    expect(sections.contains(QStringLiteral("Transform")) && sections.contains(QStringLiteral("RigidBody")) &&
               sections.contains(QStringLiteral("PointLight")),
           "inspector: one editable section per component of the selection");
    expect(inspector->fieldSpin(QStringLiteral("RigidBody.mass")) != nullptr &&
               inspector->fieldSpin(QStringLiteral("transform.rotation"), 2) != nullptr &&
               inspector->fieldSpin(QStringLiteral("PointLight.color"), 1) != nullptr &&
               inspector->fieldCheck(QStringLiteral("RigidBody.is_static")) != nullptr,
           "inspector: typed editors (float, Euler, colour, bool) generated from the component schema");
    expect(inspector->removeButton(QStringLiteral("Transform")) == nullptr &&
               inspector->removeButton(QStringLiteral("RigidBody")) != nullptr,
           "inspector: Remove on every section but Transform");

    // Spin-box drag on the mass: every step posts SetProperty; the stack keeps one undo step.
    const auto depth0 = window.host().commandStack().undoDepth();
    QDoubleSpinBox* mass = inspector->fieldSpin(QStringLiteral("RigidBody.mass"));
    for (int i = 2; i <= 8; ++i) {
        mass->setValue(i);
        if (i % 3 == 0) {
            tick(window); // the drag spans several game ticks
            mass = inspector->fieldSpin(QStringLiteral("RigidBody.mass"));
        }
    }
    mass->setFocus();
    QTest::keyClick(mass, Qt::Key_Up); // + one keyboard step (0.1)
    tick(window);
    const ecs::RigidBody body = component<ecs::RigidBody>(window, e);
    expect(closeTo(body.mass, 8.1f) && closeTo(body.inv_mass, 1.f / 8.1f), "inspector: mass drag reached the registry");
    expect(window.host().commandStack().undoDepth() == depth0 + 1u, "inspector: the whole drag is one undo step");
    expect(window.host().isSceneDirty() && window.windowTitle().contains(QLatin1Char('*')),
           "inspector: edit marks the scene dirty (title '*')");
    mass->clearFocus();

    QAction* undo = editAction(window, QStringLiteral("&Undo"));
    QAction* redo = editAction(window, QStringLiteral("&Redo"));
    expect(undo != nullptr && redo != nullptr, "Edit > Undo / Redo present");
    if (undo == nullptr || redo == nullptr) {
        return 1;
    }
    undo->trigger();
    tick(window);
    expect(component<ecs::RigidBody>(window, e).mass == 1.f &&
               inspector->fieldSpin(QStringLiteral("RigidBody.mass"))->value() == 1.0,
           "inspector: Edit > Undo restores the pre-drag value (registry + editor)");
    redo->trigger();
    tick(window);
    expect(closeTo(component<ecs::RigidBody>(window, e).mass, 8.1f), "inspector: Edit > Redo re-applies it");

    // Euler rotation (degrees in the UI, quaternion in the command) and colour.
    inspector->fieldSpin(QStringLiteral("transform.rotation"), 1)->setValue(90.0);
    inspector->fieldSpin(QStringLiteral("PointLight.color"), 1)->setValue(0.25);
    tick(window);
    const ecs::Transform t = component<ecs::Transform>(window, e);
    expect(closeTo(t.rotation.y, std::sqrt(0.5f)) && closeTo(t.rotation.w, std::sqrt(0.5f)) && t.dirty,
           "inspector: Euler Y = 90 deg edits the quaternion (Transform dirty -> live viewport update)");
    expect(component<ecs::PointLight>(window, e).color.y == 0.25f, "inspector: colour edit");

    // Bool.
    inspector->fieldCheck(QStringLiteral("RigidBody.is_static"))->click();
    tick(window);
    expect(component<ecs::RigidBody>(window, e).is_static, "inspector: bool edit");

    // Edit > Undo walks both histories newest first: bool, colour, rotation, then the mass drag.
    undo->trigger();
    undo->trigger();
    undo->trigger();
    tick(window);
    const ecs::Transform t0 = component<ecs::Transform>(window, e);
    expect(!component<ecs::RigidBody>(window, e).is_static && component<ecs::PointLight>(window, e).color.y == 1.f &&
               t0.rotation.w == 1.f && t0.rotation.y == 0.f,
           "inspector: undo reverts bool, colour and rotation in order");

    // Add Component menu -> AddComponent; enum edit; Remove -> RemoveComponent; undo restores.
    QMenu* addMenu = inspector->addComponentMenu();
    QAction* addCollider = nullptr;
    for (QAction* a : addMenu->actions()) {
        if (a->data().toString() == QLatin1String("Collider")) {
            addCollider = a;
        }
    }
    expect(addCollider != nullptr, "inspector: Add Component menu offers the missing components");
    if (addCollider == nullptr) {
        return 1;
    }
    const auto undoScene0 = window.host().undoStack().undoCount();
    addCollider->trigger();
    tick(window);
    expect(hasComponent<ecs::Collider>(window, e) && window.host().undoStack().undoCount() == undoScene0 + 1u,
           "inspector: Add Component posts an undoable AddComponent");
    QComboBox* shape = inspector->fieldCombo(QStringLiteral("Collider.shape"));
    expect(shape != nullptr && shape->count() == 8, "inspector: enum editor for Collider.shape");
    if (shape == nullptr) {
        return 1;
    }
    shape->setCurrentIndex(static_cast<int>(ecs::Collider::Capsule));
    inspector->fieldSpin(QStringLiteral("Collider.friction_static"))->setValue(0.9);
    tick(window);
    const ecs::Collider edited = component<ecs::Collider>(window, e);
    expect(edited.shape == ecs::Collider::Capsule && closeTo(edited.friction_static, 0.9f), "inspector: enum + float edit");
    QToolButton* remove = inspector->removeButton(QStringLiteral("Collider"));
    expect(remove != nullptr, "inspector: Collider Remove button");
    if (remove != nullptr) {
        remove->click();
    }
    tick(window);
    expect(!hasComponent<ecs::Collider>(window, e) && !inspector->componentSections().contains(QStringLiteral("Collider")),
           "inspector: Remove posts RemoveComponent, the section goes away");
    undo->trigger();
    tick(window);
    const ecs::Collider restored = component<ecs::Collider>(window, e);
    expect(hasComponent<ecs::Collider>(window, e) && std::memcmp(&restored, &edited, sizeof(edited)) == 0,
           "inspector: Edit > Undo restores the removed component byte-for-byte");

    // Script (E10) asset field.
    expect(inspector->addComponent(QStringLiteral("Script")), "inspector: add Script");
    tick(window);
    QLineEdit* path = inspector->fieldText(QStringLiteral("Script.script_path"));
    expect(path != nullptr, "inspector: asset-path editor for Script.script_path");
    if (path != nullptr) {
        path->setText(QStringLiteral("scripts/spinner.lua"));
        emit path->editingFinished();
        tick(window);
        expect(component<ecs::Script>(window, e).path() == "scripts/spinner.lua", "inspector: script path edit");
    }

    // Position through the vec3 editor.
    inspector->fieldSpin(QStringLiteral("transform.position"), 0)->setValue(-4.5);
    tick(window);
    expect(component<ecs::Transform>(window, e).position.x == -4.5f, "inspector: vec3 position edit");
    return 0;
}

// ---- reparent -------------------------------------------------------------------------------------

bool sameMatrix(const ecs::mat4& a, const ecs::mat4& b) {
    for (int i = 0; i < 16; ++i) {
        if (!closeTo(a.data[static_cast<std::size_t>(i)], b.data[static_cast<std::size_t>(i)], 1e-4f)) {
            return false;
        }
    }
    return true;
}

ecs::mat4 worldOf(MainWindow& window, ecs::EntityID id) {
    std::lock_guard<std::mutex> lock(window.sceneMutex());
    return fuse::editor::entityWorldMatrix(window.host().editorScene().registry(), id);
}

void syntheticDrop(QTreeWidget* tree, QTreeWidgetItem* dragged, const QPoint& at) {
    tree->clearSelection();
    dragged->setSelected(true);
    tree->setCurrentItem(dragged);
    QMimeData* mime = tree->model()->mimeData(tree->selectionModel()->selectedIndexes());
    // Enter -> move -> drop, as a mouse drag delivers them (QApplication routes the drop to the
    // widget that accepted the enter).
    QDragEnterEvent enter(at, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(at, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &move);
    QDropEvent drop(QPointF(at), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &drop);
    delete mime;
}

int gateReparent(MainWindow& window) {
    HierarchyWidget* hierarchy = window.hierarchy();
    const ecs::EntityID parent = makeEntity(window, {10.f, 1.f, -2.f});
    const ecs::EntityID child = makeEntity(window, {1.f, 2.f, 3.f});
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& registry = window.host().editorScene().registry();
        ecs::Transform* p = registry.get<ecs::Transform>(parent);
        p->rotation = fuse::editor::quatFromEulerDeg(0.f, 90.f, 30.f);
        p->scale = {2.f, 2.f, 2.f, 0.f};
        registry.get<ecs::Transform>(child)->rotation = fuse::editor::quatFromEulerDeg(15.f, 0.f, 0.f);
    }
    window.refreshPanels();
    const ecs::Transform before = component<ecs::Transform>(window, child);
    const ecs::mat4 world0 = worldOf(window, child);

    QTreeWidget* tree = hierarchy->tree();
    expect(tree->dragDropMode() == QAbstractItemView::InternalMove, "reparent: hierarchy tree is internal-move DnD");
    QTreeWidgetItem* childRow = hierarchy->itemFor(child);
    QTreeWidgetItem* parentRow = hierarchy->itemFor(parent);
    expect(childRow != nullptr && parentRow != nullptr, "reparent: rows for both entities");
    if (childRow == nullptr || parentRow == nullptr) {
        return 1;
    }
    const auto undo0 = window.host().undoStack().undoCount();
    syntheticDrop(tree, childRow, tree->visualItemRect(parentRow).center());
    tick(window);
    expect(component<ecs::Transform>(window, child).parent == parent, "reparent: drop on a row parents under it");
    expect(sameMatrix(worldOf(window, child), world0), "reparent: world pose kept");
    expect(window.host().undoStack().undoCount() == undo0 + 1u, "reparent: one undo step");
    QTreeWidgetItem* moved = hierarchy->itemFor(child);
    expect(moved != nullptr && moved->parent() == hierarchy->itemFor(parent), "reparent: hierarchy shows the new parent");

    QAction* undo = editAction(window, QStringLiteral("&Undo"));
    undo->trigger();
    tick(window);
    const ecs::Transform after = component<ecs::Transform>(window, child);
    expect(!after.parent.valid() && std::memcmp(&after.position, &before.position, sizeof(before.position)) == 0 &&
               std::memcmp(&after.rotation, &before.rotation, sizeof(before.rotation)) == 0,
           "reparent: Edit > Undo restores root parent and exact local TRS");

    // Re-parent, then drop on empty space: back to the root, world pose kept.
    editAction(window, QStringLiteral("&Redo"))->trigger();
    tick(window);
    QTreeWidgetItem* again = hierarchy->itemFor(child);
    const QRect viewportRect = tree->viewport()->rect();
    syntheticDrop(tree, again, QPoint(viewportRect.center().x(), viewportRect.bottom() - 2));
    tick(window);
    expect(!component<ecs::Transform>(window, child).parent.valid() && sameMatrix(worldOf(window, child), world0),
           "reparent: drop on empty space un-parents keeping the world pose");

    // A drop onto its own descendant is refused (no cycle, no undo step).
    syntheticDrop(tree, hierarchy->itemFor(child), tree->visualItemRect(hierarchy->itemFor(parent)).center());
    tick(window);
    const auto undo1 = window.host().undoStack().undoCount();
    syntheticDrop(tree, hierarchy->itemFor(parent), tree->visualItemRect(hierarchy->itemFor(child)).center());
    tick(window);
    expect(!component<ecs::Transform>(window, parent).parent.valid() && window.host().undoStack().undoCount() == undo1,
           "reparent: dropping a parent onto its child is refused");
    return 0;
}

// ---- File menu ------------------------------------------------------------------------------------

int gateFileMenu(MainWindow& window, const std::filesystem::path& dir) {
    QAction* newScene = window.fileAction(MainWindow::kActionNewScene);
    QAction* open = window.fileAction(MainWindow::kActionOpenScene);
    QAction* save = window.fileAction(MainWindow::kActionSave);
    QAction* saveAs = window.fileAction(MainWindow::kActionSaveAs);
    expect(newScene && open && save && saveAs && window.fileAction(MainWindow::kActionQuit) &&
               window.fileAction(MainWindow::kActionNewProject) && window.recentFilesMenu() != nullptr,
           "file: New Scene / Open / Open Recent / Save / Save As / New Project / Quit in the File menu");
    if (!newScene || !open || !save || !saveAs) {
        return 1;
    }
    QMessageBox::StandardButton answer = QMessageBox::Cancel;
    QString lastPrompt;
    window.setDirtyPromptHook([&](const QString& text) {
        lastPrompt = text;
        return answer;
    });
    QString dialogPath;
    window.setFileDialogHook([&](bool) { return dialogPath; });

    // Start clean (discard whatever an earlier gate left).
    answer = QMessageBox::Discard;
    newScene->trigger();
    QCoreApplication::processEvents();
    expect(!window.host().isSceneDirty() && !window.windowTitle().contains(QLatin1Char('*')),
           "file: New Scene gives a clean untitled scene");
    expect(window.windowTitle().startsWith(window.sceneDisplayName()), "file: title shows the scene name");

    // Edit: entity + component + property, all through the command queue.
    const ecs::EntityID e = makeEntity(window, {7.f, 8.f, 9.f});
    const fuse::Handle<fuse::Object> handle(e.index, e.generation);
    window.host().postFromUi(fuse::editor::makeAddComponentCommand(handle, "RigidBody"));
    tick(window);
    window.host().postFromUi(fuse::editor::makeSetPropertyCommand(handle, "RigidBody.mass", "42"));
    tick(window);
    expect(window.host().isSceneDirty() && window.windowTitle().contains(QLatin1Char('*')), "file: edit -> title '*'");

    // Save (no path yet) goes through Save As.
    const QString levelPath = QString::fromStdString((dir / "e20_scene.fuselevel").string());
    dialogPath = levelPath;
    save->trigger();
    QCoreApplication::processEvents();
    expect(std::filesystem::exists(dir / "e20_scene.fuselevel") && !window.host().isSceneDirty(),
           "file: Save on an untitled scene asks for a path and saves");
    expect(window.windowTitle().startsWith(QStringLiteral("e20_scene - ")) && !window.windowTitle().contains(QLatin1Char('*')),
           "file: title shows the saved scene name without '*'");
    expect(!window.recentFiles().isEmpty() && window.recentFiles().front().endsWith(QStringLiteral("e20_scene.fuselevel")),
           "file: saved scene heads Recent Files");

    // New (clean -> no prompt), then Open Recent shows the saved entity.
    const int prompts0 = window.dirtyPromptCount();
    newScene->trigger();
    QCoreApplication::processEvents();
    expect(window.dirtyPromptCount() == prompts0, "file: no prompt when the scene is clean");
    expect(window.host().editorScene().registry().count() == 0u, "file: New Scene empties the scene");
    QAction* recent = window.recentFilesMenu()->actions().isEmpty() ? nullptr : window.recentFilesMenu()->actions().front();
    expect(recent != nullptr, "file: Open Recent entry");
    if (recent != nullptr) {
        recent->trigger();
        QCoreApplication::processEvents();
    }
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().editorScene().registry().each<ecs::RigidBody>([&](ecs::EntityID id, ecs::RigidBody& body) {
            const ecs::Transform* t = window.host().editorScene().registry().get<ecs::Transform>(id);
            found = found || (body.mass == 42.f && t != nullptr && t->position.x == 7.f);
        });
    }
    expect(found, "file: reopened scene shows the saved entity (RigidBody mass 42 at x = 7)");
    window.refreshPanels();
    expect(window.hierarchy()->entityRowCount() >= 1, "file: hierarchy lists the reopened entity");

    // Dirty prompt: Cancel keeps the edit, Save writes it, Discard drops it.
    const ecs::EntityID extra = makeEntity(window, {0.f, 0.f, 0.f});
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().editorState().sceneModified = true;
    }
    answer = QMessageBox::Cancel;
    newScene->trigger();
    QCoreApplication::processEvents();
    expect(window.dirtyPromptCount() == prompts0 + 1 && lastPrompt.contains(QStringLiteral("e20_scene")),
           "file: New on a dirty scene asks to save (prompt names the scene)");
    expect(window.host().editorScene().registry().alive(extra), "file: Cancel keeps the scene");

    answer = QMessageBox::Cancel;
    QCloseEvent close;
    QCoreApplication::sendEvent(&window, &close);
    expect(!close.isAccepted() && window.dirtyPromptCount() == prompts0 + 2, "file: closing a dirty window prompts; Cancel keeps it open");

    answer = QMessageBox::Save;
    dialogPath = levelPath;
    open->trigger(); // prompt -> Save (to the current path) -> open the dialog's file
    QCoreApplication::processEvents();
    expect(window.dirtyPromptCount() == prompts0 + 3 && !window.host().isSceneDirty(), "file: Save in the prompt saves, then opens");
    expect(window.host().editorScene().registry().count() == 2u, "file: the saved extra entity is in the reopened file");

    makeEntity(window, {1.f, 1.f, 1.f});
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().editorState().sceneModified = true;
    }
    answer = QMessageBox::Discard;
    newScene->trigger();
    QCoreApplication::processEvents();
    expect(window.dirtyPromptCount() == prompts0 + 4 && window.host().editorScene().registry().count() == 0u &&
               !window.host().isSceneDirty(),
           "file: Discard drops the edit and runs New");

    // Save As to a second file keeps both in Recent Files, newest first.
    dialogPath = QString::fromStdString((dir / "second.fuselevel").string());
    saveAs->trigger();
    QCoreApplication::processEvents();
    expect(window.recentFiles().size() >= 2 && window.recentFiles().front().endsWith(QStringLiteral("second.fuselevel")),
           "file: Save As adds to Recent Files");
    window.setDirtyPromptHook(nullptr);
    window.setFileDialogHook(nullptr);
    return 0;
}

// ---- New Project wizard ---------------------------------------------------------------------------

int gateWizard(MainWindow& window, const std::filesystem::path& dir) {
    auto* hub = window.projectHub();
    window.setDirtyPromptHook([](const QString&) { return QMessageBox::Discard; });
    hub->setWizardHook([&](NewProjectWizard& wizard) {
        wizard.nameEdit()->setText(QStringLiteral("E20 Wizard"));
        wizard.locationEdit()->setText(QString::fromStdString(dir.string()));
        wizard.dimensionButton(NewProjectWizard::Dimension::World2D)->setChecked(true);
        wizard.uiCheck()->setChecked(false);
        wizard.moduleCheck(QStringLiteral("ai"))->setChecked(true);
        wizard.moduleCheck(QStringLiteral("fx"))->setChecked(true);
        return true;
    });
    QAction* newProject = window.fileAction(MainWindow::kActionNewProject);
    expect(newProject != nullptr, "wizard: File > New Project");
    if (newProject == nullptr) {
        return 1;
    }
    newProject->trigger();
    QCoreApplication::processEvents();
    const std::filesystem::path projectDir = dir / "E20 Wizard";
    expect(std::filesystem::exists(projectDir / "project.json"), "wizard: project.json written");
    const fuse::project::LoadResult loaded = fuse::project::loadFromDirectory(projectDir.string());
    expect(loaded.status == fuse::project::LoadStatus::Ok && loaded.manifest.name == "E20 Wizard",
           "wizard: manifest loads with the chosen name");
    const auto& m = loaded.manifest;
    expect(!m.dimensions.enable3D && m.dimensions.enable2D && !m.dimensions.enableUI && m.defaultWorld3D.empty() &&
               !m.defaultWorld2D.empty(),
           "wizard: 2D dimension, no UI layer");
    expect(m.modules.ai && m.modules.fx && !m.modules.cinematics && !m.modules.mechanics && !m.modules.adventure,
           "wizard: module toggles written to project.json");
    expect(window.host().hasProject() && window.host().projectManifest().modules.ai &&
               window.host().currentSceneDimension() == fuse::scene::SceneDimension::World2D,
           "wizard: the editor opened the new project (2D default world)");
    expect(window.windowTitle().contains(QStringLiteral("E20 Wizard")), "wizard: title names the project");
    expect(hub->selectedProjectPath().endsWith(QStringLiteral("E20 Wizard")), "wizard: project hub lists and selects it");

    fuse::editor::EditorHost fresh;
    expect(fresh.openProject(projectDir.string()) && fresh.hasProject() &&
               fresh.currentScenePath().find("main2d.fuselevel") != std::string::npos,
           "wizard: the project opens in a fresh EditorHost (loadable)");

    // Creating it again fails cleanly (project.json exists) without touching the open project.
    newProject->trigger();
    QCoreApplication::processEvents();
    expect(!window.host().lastFileResult().ok && window.host().hasProject(), "wizard: existing project refused");
    hub->setWizardHook(nullptr);
    window.setDirtyPromptHook(nullptr);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    fuse::core::initialize();
    int code = 0;
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("fuse_e20_qt");
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
        if (gate == "inspector" || gate == "all") {
            code |= gateInspector(window);
        }
        if (gate == "reparent" || gate == "all") {
            code |= gateReparent(window);
        }
        if (gate == "file_menu" || gate == "all") {
            code |= gateFileMenu(window, dir);
        }
        if (gate == "wizard" || gate == "all") {
            code |= gateWizard(window, dir);
        }
        // Leave the window clean so teardown never prompts.
        window.setDirtyPromptHook([](const QString&) { return QMessageBox::Discard; });
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    fuse::core::shutdown();
    std::printf("qt inspector/files: %s (%d failure(s))\n", g_failures == 0 && code == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 && code == 0 ? 0 : 1;
}
