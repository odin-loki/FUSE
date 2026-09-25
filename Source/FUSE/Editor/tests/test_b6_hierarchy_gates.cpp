// B6.5 / B6.13 gates — scene hierarchy model: structure, reparent commands, cycles, ordering, search.
#include <fuse/core/init.hpp>
#include <fuse/editor/hierarchy_model.hpp>
#include <fuse/editor/scene_hierarchy_panel.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/object.hpp>
#include <fuse/scene/scene.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

std::string rowsToString(const std::vector<fuse::editor::HierarchyNode>& rows) {
    std::string text;
    for (const fuse::editor::HierarchyNode& row : rows) {
        text += std::to_string(row.depth) + ":" + row.name + " ";
    }
    return text;
}

std::string childNames(const fuse::Object& parent) {
    std::string text;
    for (const fuse::Object* child : parent.children()) {
        text += child->name() + " ";
    }
    return text;
}

// Gate: all entities displayed — tree structure (depth-first rows, sibling order) matches links.
void testTreeMatchesParentChild() {
    TestObject root("Root", 1u);
    TestObject a("A", 2u);
    TestObject b("B", 3u);
    TestObject c("C", 4u);
    TestObject a1("A1", 5u);
    TestObject a2("A2", 6u);
    TestObject a2x("A2x", 7u);
    root.addChild(&a);
    root.addChild(&b);
    root.addChild(&c);
    a.addChild(&a1);
    a.addChild(&a2);
    a2.addChild(&a2x);

    fuse::editor::HierarchyModel model;
    model.setRoot(&root);
    expectTrue(rowsToString(model.flatNodes()) == "0:Root 1:A 2:A1 2:A2 3:A2x 1:B 1:C ",
               "rows are depth-first with sibling order and depths matching the tree");
    expectTrue(model.childrenOf(a.handle()).size() == 2u && model.childrenOf(a2.handle()).size() == 1u &&
                   model.childrenOf(b.handle()).empty(),
               "childrenOf returns direct children only");

    // Runtime scene source: parentIndex links, including a corrupt self/cycle link.
    fuse::scene::Scene scene("gates");
    scene.addEntity("World");            // 0
    scene.addEntity("Lamp", {}, 0);      // 1 -> World
    scene.addEntity("Bulb", {}, 1);      // 2 -> Lamp
    scene.addEntity("LoopA", {}, 4);     // 3 -> LoopB
    scene.addEntity("LoopB", {}, 3);     // 4 -> LoopA (cycle in data)
    fuse::editor::HierarchyModel sceneModel;
    sceneModel.setScene(&scene);
    expectTrue(sceneModel.flatNodes().size() == 5u, "every scene entity appears exactly once, even in a data cycle");
    fuse::editor::HierarchyNode bulb;
    expectTrue(sceneModel.findNamed("Bulb", bulb) && bulb.depth == 2u, "scene parentIndex depth honoured");
}

// Gate: drag-and-drop reparent emits a ReparentCommand and updates the hierarchy immediately.
void testReparentEmitsCommandAndRefreshes() {
    TestObject root("Root", 1u);
    TestObject a("A", 2u);
    TestObject b("B", 3u);
    root.addChild(&a);
    root.addChild(&b);

    fuse::editor::SceneHierarchyPanel panel;
    fuse::editor::UndoStack stack;
    panel.setSceneRoot(&root);
    panel.select(b.handle());
    panel.reparentSelection(&a, stack);

    expectTrue(stack.undoCount() == 1u && stack.peekUndoDescription() == "Reparent B",
               "reparent recorded as one ReparentObjectCommand");
    expectTrue(b.parent() == &a, "reparent applied");
    expectTrue(rowsToString(panel.model().flatNodes()) == "0:Root 1:A 2:B ", "model refreshed immediately");

    stack.undo();
    panel.refresh();
    expectTrue(b.parent() == &root && rowsToString(panel.model().flatNodes()) == "0:Root 1:A 1:B ",
               "undo restores the original tree");

    panel.select(b.handle());
    panel.reparentSelection(&root, stack);
    expectTrue(stack.undoCount() == 0u, "reparent onto the current parent records nothing");
}

// Cycles: a node may never be dropped onto itself or its own subtree.
void testCyclesPrevented() {
    TestObject root("Root", 1u);
    TestObject a("A", 2u);
    TestObject a1("A1", 3u);
    TestObject a1x("A1x", 4u);
    root.addChild(&a);
    a.addChild(&a1);
    a1.addChild(&a1x);

    fuse::editor::SceneHierarchyPanel panel;
    fuse::editor::UndoStack stack;
    panel.setSceneRoot(&root);
    panel.select(a.handle());
    panel.reparentSelection(&a1x, stack);
    expectTrue(stack.undoCount() == 0u, "panel rejects drop onto a descendant");
    expectTrue(a.parent() == &root && a1x.parent() == &a1, "tree unchanged after rejected drop");

    fuse::editor::ReparentObjectCommand direct(a, &a1, &root);
    expectTrue(!direct.isValid(), "command reports the cycle");
    direct.execute();
    expectTrue(a.parent() == &root && a1.parent() == &a, "cyclic command execute is a no-op");
    direct.undo();
    expectTrue(a.parent() == &root, "undo of a rejected command is a no-op");

    // ECS: Transform::parent chains.
    fuse::ecs::Registry registry;
    registry.init(16);
    const fuse::ecs::EntityID top = registry.create();
    const fuse::ecs::EntityID mid = registry.create();
    const fuse::ecs::EntityID leaf = registry.create();
    registry.add(top, fuse::ecs::Transform{});
    fuse::ecs::Transform midT{};
    midT.parent = top;
    registry.add(mid, midT);
    fuse::ecs::Transform leafT{};
    leafT.parent = mid;
    registry.add(leaf, leafT);

    expectTrue(fuse::editor::wouldCreateParentCycle(registry, top, leaf), "top under leaf is a cycle");
    expectTrue(fuse::editor::wouldCreateParentCycle(registry, mid, mid), "self-parent is a cycle");
    expectTrue(!fuse::editor::wouldCreateParentCycle(registry, leaf, top), "leaf under top is fine");

    stack.execute(std::make_unique<fuse::editor::ReparentEntityCommand>(registry, top, leaf,
                                                                        fuse::ecs::EntityID::null()));
    expectTrue(!registry.get<fuse::ecs::Transform>(top)->parent.valid(), "ECS cyclic reparent rejected");
    stack.undo();
    expectTrue(!registry.get<fuse::ecs::Transform>(top)->parent.valid() &&
                   registry.get<fuse::ecs::Transform>(mid)->parent == top,
               "undo of rejected ECS reparent leaves links intact");

    stack.execute(std::make_unique<fuse::editor::ReparentEntityCommand>(registry, leaf, top, mid));
    expectTrue(registry.get<fuse::ecs::Transform>(leaf)->parent == top, "valid ECS reparent applies");
    stack.undo();
    expectTrue(registry.get<fuse::ecs::Transform>(leaf)->parent == mid, "valid ECS reparent undone");
    registry.destroy();
}

// Ordering: undoing a reparent puts the node back at its original sibling index.
void testSiblingOrderRestored() {
    TestObject root("Root", 1u);
    TestObject a("A", 2u);
    TestObject b("B", 3u);
    TestObject c("C", 4u);
    TestObject d("D", 5u);
    root.addChild(&a);
    root.addChild(&b);
    root.addChild(&c);
    root.addChild(&d);

    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<fuse::editor::ReparentObjectCommand>(b, &d, &root));
    expectTrue(childNames(root) == "A C D " && childNames(d) == "B ", "B moved under D");
    stack.execute(std::make_unique<fuse::editor::ReparentObjectCommand>(a, nullptr, &root));
    expectTrue(a.parent() == nullptr && childNames(root) == "C D ", "A unparented");

    stack.undo();
    expectTrue(childNames(root) == "A C D ", "A restored at index 0");
    stack.undo();
    expectTrue(childNames(root) == "A B C D ", "B restored at its original index 1");
    stack.redo();
    stack.redo();
    expectTrue(childNames(root) == "C D " && childNames(d) == "B ", "redo replays both moves");
}

// Gate: search filter shows only matching entities — case-insensitive.
void testSearchCaseInsensitive() {
    TestObject root("Level", 1u);
    TestObject lamp("StreetLamp", 2u);
    TestObject lamp2("lamp_post", 3u);
    TestObject tree("OakTree", 4u);
    root.addChild(&lamp);
    root.addChild(&lamp2);
    root.addChild(&tree);

    fuse::editor::SceneHierarchyPanel panel;
    panel.setSceneRoot(&root);
    panel.setSearchQuery("LAMP");
    const std::vector<fuse::editor::HierarchyNode> rows = panel.model().visibleNodes();
    expectTrue(rows.size() == 2u && rows[0].name == "StreetLamp" && rows[1].name == "lamp_post",
               "upper-case query matches mixed-case names only");
    panel.setSearchQuery("oAkT");
    expectTrue(panel.visibleNodeCount() == 1u, "mixed-case partial query matches");
    panel.setSearchQuery("");
    expectTrue(panel.visibleNodeCount() == 4u, "empty query shows everything");
    panel.setSearchQuery("zzz");
    expectTrue(panel.visibleNodeCount() == 0u, "non-matching query shows nothing");
}

} // namespace

int main() {
    fuse::core::initialize();

    testTreeMatchesParentChild();
    testReparentEmitsCommandAndRefreshes();
    testCyclesPrevented();
    testSiblingOrderRestored();
    testSearchCaseInsensitive();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_hierarchy_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_hierarchy_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
