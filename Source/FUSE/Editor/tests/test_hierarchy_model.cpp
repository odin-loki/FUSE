#include <fuse/core/init.hpp>
#include <fuse/editor/hierarchy_row.hpp>
#include <fuse/editor/sibling_index.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/scene_hierarchy_panel.hpp>
#include <fuse/object.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

class TestObject final : public fuse::Object {
public:
    TestObject(std::string name, fuse::u32 index) : Object(std::move(name)) {
        setHandle(fuse::Handle<fuse::Object>(index, 1u));
    }
};

void testHierarchyRowOk() {
    expectTrue(fuse::editor::hierarchy_row_ok(0, 1), "hierarchy_row_ok(0, 1)");
    expectTrue(fuse::editor::hierarchy_row_ok(2, 3), "hierarchy_row_ok(2, 3)");
    expectTrue(!fuse::editor::hierarchy_row_ok(0, 0), "hierarchy_row_ok(0, 0)");
    expectTrue(!fuse::editor::hierarchy_row_ok(-1, 4), "hierarchy_row_ok(-1, 4)");
    expectTrue(!fuse::editor::hierarchy_row_ok(4, 4), "hierarchy_row_ok(4, 4)");
    expectTrue(fuse::editor::hierarchy_row_ok(99999, 100000), "hierarchy_row_ok(99999, 100000)");
    expectTrue(!fuse::editor::hierarchy_row_ok(0, 100001), "hierarchy_row_ok(0, 100001)");
}

void testHierarchyModelFlatten() {
    TestObject root("root", 1u);
    fuse::SceneObject3D child("child3d");
    fuse::SceneObject2D sibling("sibling2d");

    root.addChild(&child);
    root.addChild(&sibling);

    fuse::editor::HierarchyModel model;
    model.setRoot(&root);

    expectTrue(model.flatNodes().size() == 3u, "root and two children flattened");
    expectTrue(model.childrenOf(root.handle()).size() == 2u, "root has two children");
}

void testHierarchyModelFromScene() {
    fuse::scene::Scene scene("p5");
    scene.addEntity("Floor");
    scene.addEntity("CameraSpawnPoints");
    scene.addEntity("DefaultCameraSpawnSphere", {}, 1);

    fuse::editor::HierarchyModel model;
    model.setScene(&scene);

    expectTrue(model.root() == nullptr, "Object root stays empty for scene source");
    expectTrue(model.flatNodes().size() == 3u, "scene entities listed when Object root is empty");

    fuse::editor::HierarchyNode floor;
    expectTrue(model.findNamed("Floor", floor), "Floor row present");
    expectTrue(floor.handle.index() == 0u, "Floor row index matches entity");
    expectTrue(floor.depth == 0u, "Floor is a root-level row");

    fuse::editor::HierarchyNode spawn;
    expectTrue(model.findNamed("DefaultCameraSpawnSphere", spawn), "child entity listed");
    expectTrue(spawn.depth == 1u, "child row uses parentIndex depth");
}

void testHierarchySearchFilter() {
    TestObject root("root", 1u);
    TestObject camera("MainCamera", 2u);
    TestObject light("PointLight", 3u);

    root.addChild(&camera);
    root.addChild(&light);

    fuse::editor::SceneHierarchyPanel panel;
    panel.setSceneRoot(&root);
    panel.setSearchQuery("camera");

    expectTrue(panel.visibleNodeCount() == 1u, "search filters to matching node");
}

void testHierarchyReparentThroughPanel() {
    TestObject root("root", 1u);
    TestObject group("group", 2u);
    TestObject child("child", 3u);

    root.addChild(&group);
    root.addChild(&child);

    fuse::editor::SceneHierarchyPanel panel;
    fuse::editor::UndoStack stack;
    panel.setSceneRoot(&root);
    panel.select(child.handle());
    panel.reparentSelection(&group, stack);

    expectTrue(child.parent() == &group, "panel reparent applies");
    expectTrue(stack.undoCount() == 1u, "reparent recorded on command stack");

    stack.undo();
    expectTrue(child.parent() == &root, "panel reparent undone");
}

void testHierarchySiblingIndex() {
    expectTrue(fuse::editor::sibling_index(1, 3) == 1, "index 1 of 3 is 1");
    expectTrue(fuse::editor::sibling_index(3, 3) == -1, "index 3 of 3 is -1");
    expectTrue(fuse::editor::sibling_wrap(0, -1, 3) == 2, "wrap(0, -1, 3) == 2");
}

} // namespace

int main() {
    testHierarchyRowOk();
    fuse::core::initialize();
    testHierarchyModelFlatten();
    testHierarchyModelFromScene();
    testHierarchySearchFilter();
    testHierarchyReparentThroughPanel();
    testHierarchySiblingIndex();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_hierarchy_model_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_hierarchy_model_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
