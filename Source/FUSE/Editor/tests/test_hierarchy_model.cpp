#include <fuse/core/init.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/scene_hierarchy_panel.hpp>
#include <fuse/object.hpp>
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

} // namespace

int main() {
    fuse::core::initialize();
    testHierarchyModelFlatten();
    testHierarchySearchFilter();
    testHierarchyReparentThroughPanel();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_hierarchy_model_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_hierarchy_model_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
