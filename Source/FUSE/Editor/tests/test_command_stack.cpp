#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/object.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

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

class CounterCommand final : public fuse::editor::UndoCommand {
public:
    CounterCommand(int& counter, int before, int after, std::string label)
        : m_counter(counter), m_before(before), m_after(after), m_label(std::move(label)) {}

    void execute() override { m_counter = m_after; }
    void undo() override { m_counter = m_before; }
    std::string description() const override { return m_label; }

    bool merge(const fuse::editor::UndoCommand& other) override {
        const auto* typed = dynamic_cast<const CounterCommand*>(&other);
        if (!typed || typed->m_label != m_label) {
            return false;
        }
        m_after = typed->m_after;
        return true;
    }

private:
    int& m_counter;
    int m_before;
    int m_after;
    std::string m_label;
};

fuse::editor::EditorCommand makeSetPropertyCommand(fuse::u32 targetIndex, const char* propertyName,
                                                   const char* propertyValue) {
    fuse::editor::EditorCommand command;
    command.kind = fuse::editor::CommandKind::SetProperty;
    command.target = fuse::Handle<fuse::Object>(targetIndex, 1u);
    command.propertyName = propertyName;
    command.propertyValue = propertyValue;
    return command;
}

void testUndoRedoLifo() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "add one"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "add two"));
    expectTrue(counter == 3, "execute applies in order");
    expectTrue(stack.undoCount() == 2u, "two undo steps");

    stack.undo();
    expectTrue(counter == 1, "undo removes last command");
    expectTrue(stack.canRedo(), "redo available after undo");

    stack.redo();
    expectTrue(counter == 3, "redo restores command");
}

void testMergeConsecutiveCommands() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 3, 6, "drag"));

    expectTrue(counter == 6, "merged drag deltas apply");
    expectTrue(stack.undoCount() == 1u, "merged commands collapse to one undo step");

    stack.undo();
    expectTrue(counter == 0, "single undo restores pre-drag state");
}

void testSetObjectNameCommand() {
    TestObject object("before", 1u);
    fuse::editor::UndoStack stack;

    stack.execute(std::make_unique<fuse::editor::SetObjectNameCommand>(object, "before", "after"));
    expectTrue(object.name() == "after", "rename applied");

    stack.undo();
    expectTrue(object.name() == "before", "rename undone");
}

void testReparentObjectCommand() {
    TestObject root("root", 1u);
    TestObject child("child", 2u);
    TestObject newParent("new_parent", 3u);

    root.addChild(&child);
    fuse::editor::UndoStack stack;

    stack.execute(std::make_unique<fuse::editor::ReparentObjectCommand>(child, &newParent, &root));
    expectTrue(child.parent() == &newParent, "child reparented");

    stack.undo();
    expectTrue(child.parent() == &root, "reparent undone");
}

void testUndoStackMaxHistoryEviction() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    for (int step = 0; step < fuse::editor::UndoStack::kMaxHistory + 10; ++step) {
        stack.execute(std::make_unique<CounterCommand>(counter, counter, counter + 1,
                                                       "step " + std::to_string(step)));
    }

    expectTrue(stack.undoCount() == fuse::editor::UndoStack::kMaxHistory,
               "undo stack capped at MAX_HISTORY");
    expectTrue(stack.evictedCount() == 10u, "oldest commands evicted past cap");

    for (fuse::u32 step = 0; step < fuse::editor::UndoStack::kMaxHistory; ++step) {
        stack.undo();
    }

    expectTrue(counter == 10, "undo chain restores state after oldest evictions");
    expectTrue(!stack.canUndo(), "all undo steps consumed");
}

void testUndoStackHundredCommandChain() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    for (int step = 0; step < 100; ++step) {
        stack.execute(std::make_unique<CounterCommand>(
            counter, counter, counter + 1, "increment " + std::to_string(step)));
    }

    expectTrue(counter == 100, "100 commands applied");
    expectTrue(stack.undoCount() == 100u, "100 undo steps recorded");

    for (int step = 0; step < 100; ++step) {
        stack.undo();
    }

    expectTrue(counter == 0, "100-step undo restores initial state");
    expectTrue(stack.redoCount() == 100u, "100 redo steps available");
}

void testCommandStackCoalescesPropertyEdits() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));

    expectTrue(stack.undoDepth() == 1u, "consecutive property edits coalesce to one undo step");
    expectTrue(stack.coalescedCount() == 2u, "coalesced count tracks merged commands");
    expectTrue(stack.appliedCount() == 3u, "pending queue still receives each applied value");

    const fuse::editor::EditorCommand* last = stack.lastApplied();
    expectTrue(last != nullptr && last->propertyValue == "7,8,9", "last applied value retained");

    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    expectTrue(stack.undoDepth() == 2u, "different property starts new undo step");
}

void testCommandStackDoesNotCoalesceDifferentTargets() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(2u, "transform.position", "4,5,6"));

    expectTrue(stack.undoDepth() == 2u, "different targets do not coalesce");
    expectTrue(stack.coalescedCount() == 0u, "no coalescing across targets");
}

void testCommandStackDirtyTracking() {
    fuse::editor::CommandStack stack;

    expectTrue(!stack.isDirty(), "stack starts clean");
    expectTrue(stack.dirtyRevision() == 0u, "dirty revision starts at zero");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    expectTrue(stack.isDirty(), "execute marks stack dirty");
    expectTrue(stack.dirtyRevision() == 1u, "dirty revision increments on execute");

    stack.markClean();
    expectTrue(!stack.isDirty(), "markClean clears dirty flag");
    expectTrue(stack.dirtyRevision() == 1u, "dirty revision preserved after markClean");

    stack.undo();
    expectTrue(stack.isDirty(), "undo marks stack dirty");
    expectTrue(stack.dirtyRevision() == 2u, "dirty revision increments on undo");
}

void testCommandStackSnapshotRestore() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.25"));
    stack.markClean();

    const fuse::editor::CommandStackSnapshot snapshot = stack.captureSnapshot();
    expectTrue(snapshot.undoDepth == 2u, "snapshot captures undo depth");
    expectTrue(!snapshot.dirty, "snapshot captures clean dirty flag");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "9,9,9"));
    stack.undo();
    expectTrue(stack.undoDepth() == 2u, "mutations change stack before restore");

    stack.restoreSnapshot(snapshot);
    expectTrue(stack.undoDepth() == 2u, "restore brings back undo depth");
    expectTrue(stack.redoDepth() == 0u, "restore clears redo branch");
    expectTrue(!stack.isDirty(), "restore brings back dirty flag");
    expectTrue(stack.dirtyRevision() == snapshot.dirtyRevision, "restore brings back dirty revision");

    const fuse::editor::EditorCommand* last = stack.lastApplied();
    expectTrue(last != nullptr && last->propertyName == "sdf.blend_alpha",
               "restore brings back last applied command");
}

void testCommandStackMaxHistoryEviction() {
    fuse::editor::CommandStack stack;

    for (fuse::u32 step = 0; step < fuse::editor::CommandStack::kMaxHistory + 5u; ++step) {
        stack.execute(makeSetPropertyCommand(step + 1u, "transform.position",
                                             std::to_string(step).c_str()));
    }

    expectTrue(stack.undoDepth() == fuse::editor::CommandStack::kMaxHistory,
               "command stack capped at MAX_HISTORY");
    expectTrue(stack.canUndo(), "evicted stack still supports undo");

    const fuse::editor::EditorCommand* last = stack.lastApplied();
    expectTrue(last != nullptr && last->target.index() == fuse::editor::CommandStack::kMaxHistory + 5u,
               "latest command survives eviction");
}

void testCommandStackUndoRedo() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));

    expectTrue(stack.canUndo(), "undo available after execute");
    stack.undo();
    expectTrue(stack.undoDepth() == 1u, "undo pops latest command");
    expectTrue(stack.canRedo(), "redo available after undo");

    stack.redo();
    expectTrue(stack.undoDepth() == 2u, "redo restores command");
    expectTrue(stack.lastApplied()->propertyName == "sdf.blend_alpha", "redo restores last value");
}

} // namespace

int main() {
    fuse::core::initialize();
    testUndoRedoLifo();
    testMergeConsecutiveCommands();
    testSetObjectNameCommand();
    testReparentObjectCommand();
    testUndoStackMaxHistoryEviction();
    testUndoStackHundredCommandChain();
    testCommandStackCoalescesPropertyEdits();
    testCommandStackDoesNotCoalesceDifferentTargets();
    testCommandStackDirtyTracking();
    testCommandStackSnapshotRestore();
    testCommandStackMaxHistoryEviction();
    testCommandStackUndoRedo();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_command_stack_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_command_stack_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
