#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/scene_hierarchy_panel.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/object.hpp>

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

void testUndoHierarchyViewportIntegration() {
    TestObject root("root", 1u);
    TestObject group("group", 2u);
    TestObject child("child", 3u);

    root.addChild(&group);
    root.addChild(&child);

    fuse::editor::EditorHost host;
    fuse::editor::SceneHierarchyPanel hierarchy;
    fuse::editor::ViewportPanel viewport;

    hierarchy.setSceneRoot(&root);
    viewport.setProjectLabel("Phase6Integration");
    viewport.setDimensions(1280, 720);
    viewport.clearResizeFlag();

    hierarchy.select(child.handle());
    hierarchy.reparentSelection(&group, host.undoStack());

    expectTrue(child.parent() == &group, "hierarchy reparent applies through shared undo stack");
    expectTrue(host.undoStack().undoCount() == 1u, "reparent recorded on editor host undo stack");
    expectTrue(hierarchy.visibleNodeCount() == 3u, "hierarchy shows full tree after reparent");

    viewport.tick(1.f / 60.f);
    expectTrue(viewport.tickCount() == 1u, "viewport ticks while hierarchy state is live");
    expectTrue(!viewport.needsResize(), "initial resize flag cleared");

    viewport.setDimensions(1920, 1080);
    expectTrue(viewport.needsResize(), "viewport tracks resize after hierarchy mutation");

    host.undoStack().execute(
        std::make_unique<fuse::editor::SetObjectNameCommand>(child, "child", "player"));
    hierarchy.refresh();
    hierarchy.setSearchQuery("player");
    expectTrue(hierarchy.visibleNodeCount() == 1u, "hierarchy search finds renamed node");
    expectTrue(child.name() == "player", "rename applied through shared undo stack");

    host.undoStack().undo();
    expectTrue(child.name() == "child", "undo restores renamed node");
    hierarchy.refresh();
    hierarchy.setSearchQuery("player");
    expectTrue(hierarchy.visibleNodeCount() == 0u, "search no longer matches after undo");

    host.undoStack().undo();
    expectTrue(child.parent() == &root, "undo restores original parent");
    hierarchy.refresh();
    hierarchy.setSearchQuery("");
    expectTrue(hierarchy.visibleNodeCount() == 3u, "hierarchy reflects fully undone state");
    expectTrue(!host.undoStack().canUndo(), "undo stack drained after two steps");
    expectTrue(host.undoStack().canRedo(), "redo available after undo chain");

    host.undoStack().redo();
    host.undoStack().redo();
    expectTrue(child.parent() == &group, "redo restores reparent");
    expectTrue(child.name() == "player", "redo restores rename");
}

} // namespace

int main() {
    fuse::core::initialize();
    testUndoHierarchyViewportIntegration();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_phase6_integration_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_phase6_integration_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
