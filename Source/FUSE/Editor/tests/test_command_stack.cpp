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
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "10,10,10"));
    stack.undo();
    expectTrue(stack.undoDepth() == 2u, "mutations change stack before restore");

    stack.restoreSnapshot(snapshot);
    expectTrue(stack.undoDepth() == 2u, "restore brings back undo depth");
    expectTrue(stack.redoDepth() == 0u, "restore clears redo branch");
    expectTrue(stack.coalescedCount() == snapshot.coalescedCount, "restore brings back coalesced count");
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

void testCommandStackEmptyStackNoOps() {
    fuse::editor::CommandStack stack;

    expectTrue(!stack.canUndo(), "empty stack cannot undo");
    expectTrue(!stack.canRedo(), "empty stack cannot redo");
    expectTrue(stack.lastApplied() == nullptr, "empty stack has no last applied command");

    stack.undo();
    stack.redo();

    expectTrue(stack.undoDepth() == 0u, "undo on empty stack is a no-op");
    expectTrue(stack.redoDepth() == 0u, "redo on empty stack is a no-op");
    expectTrue(stack.appliedCount() == 0u, "empty stack applied count stays zero");
}

void testUndoStackEmptyStackNoOps() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    expectTrue(!stack.canUndo(), "empty undo stack cannot undo");
    expectTrue(!stack.canRedo(), "empty undo stack cannot redo");
    expectTrue(stack.peekUndoDescription().empty(), "empty undo stack has no undo description");
    expectTrue(stack.peekRedoDescription().empty(), "empty redo stack has no redo description");

    stack.undo();
    stack.redo();

    expectTrue(counter == 0, "empty stack undo/redo does not mutate scene");
    expectTrue(stack.undoCount() == 0u, "undo on empty stack is a no-op");
    expectTrue(stack.redoCount() == 0u, "redo on empty stack is a no-op");
}

void testCommandStackPushClearsRedoBranch() {
    fuse::editor::CommandStack stack;

    stack.push(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.push(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    stack.undo();
    expectTrue(stack.canRedo(), "redo branch available after undo");

    stack.push(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.75"));
    expectTrue(!stack.canRedo(), "new push clears redo branch");
    expectTrue(stack.undoDepth() == 2u, "push after undo appends new undo step");
    expectTrue(stack.lastApplied()->propertyValue == "0.75", "push applies latest value");
}

void testCommandStackCoalesceUndoRestoresFirstValue() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));

    const fuse::editor::EditorCommand* peek = stack.peekUndo();
    expectTrue(peek != nullptr && peek->propertyValue == "7,8,9", "peek undo shows latest value");
    expectTrue(peek->propertyValueBefore == "1,2,3", "coalesce preserves drag baseline");

    stack.undo();
    expectTrue(stack.undoDepth() == 0u, "single coalesced step undoes in one pop");
    expectTrue(stack.lastApplied() == nullptr, "fully undone stack has no last applied");
    expectTrue(stack.peekUndo() == nullptr, "peek undo empty after full undo");
}

void testCommandStackPeekUndoRedo() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));

    expectTrue(stack.peekUndo()->propertyName == "sdf.blend_alpha", "peek undo returns top undo entry");
    expectTrue(stack.peekRedo() == nullptr, "peek redo empty before undo");

    stack.undo();
    expectTrue(stack.peekRedo()->propertyName == "sdf.blend_alpha", "peek redo returns undone entry");
    expectTrue(stack.peekUndo()->propertyName == "transform.position", "peek undo advances after pop");
}

void testUndoStackDirtyTracking() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    expectTrue(!stack.isDirty(), "undo stack starts clean");
    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "step"));
    expectTrue(stack.isDirty(), "execute marks undo stack dirty");
    expectTrue(stack.dirtyRevision() == 1u, "dirty revision increments on execute");

    stack.markClean();
    stack.undo();
    expectTrue(stack.isDirty(), "undo marks undo stack dirty");
    expectTrue(stack.dirtyRevision() == 2u, "dirty revision increments on undo");
}

void testCommandStackEvictedCount() {
    fuse::editor::CommandStack stack;

    for (fuse::u32 step = 0; step < fuse::editor::CommandStack::kMaxHistory + 5u; ++step) {
        stack.execute(makeSetPropertyCommand(step + 1u, "transform.position",
                                             std::to_string(step).c_str()));
    }

    expectTrue(stack.evictedCount() == 5u, "command stack tracks evicted entries");
}

void testCommandStackPreservesCallerBaseline() {
    fuse::editor::CommandStack stack;

    fuse::editor::EditorCommand command =
        makeSetPropertyCommand(1u, "transform.position", "1,2,3");
    command.propertyValueBefore = "0,0,0";
    stack.execute(std::move(command));

    const fuse::editor::EditorCommand* peek = stack.peekUndo();
    expectTrue(peek != nullptr && peek->propertyValueBefore == "0,0,0",
               "caller-supplied baseline preserved on first push");
    expectTrue(peek->propertyValue == "1,2,3", "latest value retained");
}

void testCommandStackPushWithBeforeBaseline() {
    fuse::editor::CommandStack stack;

    stack.push(makeSetPropertyCommand(1u, "transform.position", "4,5,6"), "1,2,3");
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));

    const fuse::editor::EditorCommand* peek = stack.peekUndo();
    expectTrue(peek != nullptr && peek->propertyValueBefore == "1,2,3",
               "push overload seeds coalesce baseline");
    expectTrue(peek->propertyValue == "7,8,9", "coalesced push updates latest value");
    expectTrue(stack.undoDepth() == 1u, "coalesced edits remain one undo step");
}

void testCommandStackUndoPostsInversePending() {
    fuse::editor::CommandStack stack;

    stack.push(makeSetPropertyCommand(1u, "transform.position", "1,2,3"), "0,0,0");
    expectTrue(stack.pendingQueue().pendingCount() == 1u, "execute posts forward edit");

    stack.undo();
    expectTrue(stack.pendingQueue().pendingCount() == 2u, "undo posts inverse edit");
    expectTrue(stack.appliedCount() == 2u, "undo increments applied count");
    expectTrue(stack.canRedo(), "undo leaves redo branch");
}

void testCommandStackStackDepthRoundTrip() {
    fuse::editor::CommandStack stack;

    for (fuse::u32 step = 0; step < 8u; ++step) {
        stack.push(makeSetPropertyCommand(step + 1u, "transform.position",
                                          std::to_string(step).c_str()),
                   "baseline");
    }

    expectTrue(stack.undoDepth() == 8u, "eight pushes yield depth eight");

    for (fuse::u32 step = 0; step < 4u; ++step) {
        stack.undo();
    }

    expectTrue(stack.undoDepth() == 4u, "four undos halve depth");
    expectTrue(stack.redoDepth() == 4u, "four undos populate redo branch");

    for (fuse::u32 step = 0; step < 4u; ++step) {
        stack.redo();
    }

    expectTrue(stack.undoDepth() == 8u, "four redos restore depth");
    expectTrue(stack.redoDepth() == 0u, "redo branch drained after round-trip");
}

void testUndoStackPushAlias() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.push(std::make_unique<CounterCommand>(counter, 0, 5, "push alias"));
    expectTrue(counter == 5, "push alias executes command");
    expectTrue(stack.undoCount() == 1u, "push alias records undo step");
}

void testUndoStackCoalescedOps() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 3, 6, "drag"));

    expectTrue(stack.coalescedOps() == 2u, "merged commands increment coalescedOps");
    expectTrue(stack.undoCount() == 1u, "coalesced edits remain one undo step");

    stack.execute(std::make_unique<CounterCommand>(counter, 6, 7, "other"));
    expectTrue(stack.coalescedOps() == 2u, "non-mergeable command does not bump coalescedOps");
}

void testUndoStackSetBaselineState() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "step"));
    stack.set_baseline_state();

    expectTrue(stack.isAtBaseline(), "baseline set after first command");
    expectTrue(!stack.isDirty(), "set_baseline_state clears dirty flag");
    expectTrue(!stack.hasUnsavedChanges(), "baseline matches current depth");

    stack.execute(std::make_unique<CounterCommand>(counter, 1, 2, "edit"));
    expectTrue(stack.hasUnsavedChanges(), "new command moves away from baseline");
    expectTrue(stack.isDirty(), "execute after baseline marks dirty");

    stack.undo();
    expectTrue(stack.isAtBaseline(), "undo back to baseline depth");
    expectTrue(!stack.hasUnsavedChanges(), "undo restores baseline alignment");
    expectTrue(!stack.isDirty(), "baseline dirty guard clears dirty on undo to baseline");
    expectTrue(stack.dirtyRevision() == 2u, "baseline dirty guard does not bump revision on clean undo");
}

void testUndoStackEmptyStackGuardsDirtyRevision() {
    fuse::editor::UndoStack stack;

    expectTrue(stack.dirtyRevision() == 0u, "empty stack revision starts at zero");

    stack.undo();
    stack.redo();
    stack.push(nullptr);

    expectTrue(stack.dirtyRevision() == 0u, "empty-stack undo/redo/null push do not bump revision");
    expectTrue(!stack.isDirty(), "empty-stack guards leave stack clean");
    expectTrue(stack.coalescedOps() == 0u, "null push does not increment coalescedOps");
}

void testUndoStackPushClearsRedoBranch() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.push(std::make_unique<CounterCommand>(counter, 0, 1, "first"));
    stack.push(std::make_unique<CounterCommand>(counter, 1, 2, "second"));
    stack.undo();
    expectTrue(stack.canRedo(), "redo available after undo");

    stack.push(std::make_unique<CounterCommand>(counter, 1, 4, "third"));
    expectTrue(!stack.canRedo(), "new push clears redo branch");
    expectTrue(counter == 4, "push applies latest value");
    expectTrue(stack.undoCount() == 2u, "push after undo appends undo step");
}

void testUndoStackBaselineDirtyGuardOnRedo() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "first"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 2, "second"));
    stack.set_baseline_state();

    stack.undo();
    expectTrue(stack.isDirty(), "undo away from baseline marks dirty");

    stack.redo();
    expectTrue(stack.isAtBaseline(), "redo returns to baseline depth");
    expectTrue(!stack.isDirty(), "baseline dirty guard clears dirty on redo to baseline");
    expectTrue(!stack.hasUnsavedChanges(), "redo to baseline clears unsaved flag");
}

void testUndoStackCoalescedOpsSinceBaseline() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    expectTrue(stack.coalescedOps() == 1u, "pre-baseline coalesce tracked globally");
    expectTrue(stack.coalescedOpsSinceBaseline() == 1u, "pre-baseline coalesce visible before save");

    stack.set_baseline_state();
    expectTrue(stack.coalescedOpsSinceBaseline() == 0u, "baseline resets coalesce counter");

    stack.execute(std::make_unique<CounterCommand>(counter, 3, 5, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 5, 8, "drag"));
    expectTrue(stack.coalescedOps() == 3u, "total coalesced ops accumulate");
    expectTrue(stack.coalescedOpsSinceBaseline() == 2u, "post-baseline coalesce tracked separately");
}

void testUndoStackClearOnEmptyIsNoOp() {
    fuse::editor::UndoStack stack;

    stack.clear();
    stack.clear();

    expectTrue(stack.undoCount() == 0u, "clear on empty stack stays empty");
    expectTrue(!stack.isDirty(), "clear on empty stack stays clean");
    expectTrue(stack.coalescedOpsSinceBaseline() == 0u, "clear on empty resets coalesce baseline");
}

void testUndoStackDoubleUndoEmptyGuard() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "only"));
    stack.undo();
    expectTrue(counter == 0, "first undo restores initial state");

    stack.undo();
    expectTrue(counter == 0, "second undo on empty stack is a no-op");
    expectTrue(stack.undoCount() == 0u, "undo stack remains empty");
    expectTrue(stack.redoCount() == 1u, "redo branch preserved after empty undo guard");

    stack.redo();
    stack.redo();
    expectTrue(counter == 1, "first redo restores command");
    expectTrue(stack.redoCount() == 0u, "second redo on empty stack is a no-op");
}

void testUndoStackSnapshotBaselineRoundTrip() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    stack.set_baseline_state();

    const fuse::editor::UndoStackSnapshot snapshot = stack.captureSnapshot();
    expectTrue(snapshot.coalescedOps == 1u, "snapshot captures coalescedOps");
    expectTrue(snapshot.coalescedOpsAtBaseline == 1u, "snapshot captures coalescedOpsAtBaseline");
    expectTrue(snapshot.baselineUndoCount == 1u, "snapshot captures baseline undo depth");

    stack.execute(std::make_unique<CounterCommand>(counter, 3, 5, "after save"));
    stack.restoreSnapshot(snapshot);

    expectTrue(stack.coalescedOps() == snapshot.coalescedOps, "restore brings back coalescedOps");
    expectTrue(stack.coalescedOpsSinceBaseline() == 0u, "restore brings back coalesce baseline offset");
    expectTrue(stack.isAtBaseline(), "restore brings back baseline alignment");
    expectTrue(counter == 3, "restore rewinds scene state to snapshot depth");
}

void testUndoStackIsAtBaselineUsesUndoDepthOnly() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "first"));
    stack.set_baseline_state();

    stack.execute(std::make_unique<CounterCommand>(counter, 1, 2, "edit"));
    stack.undo();
    expectTrue(stack.redoCount() == 1u, "undo leaves redo branch after edit");
    expectTrue(stack.isAtBaseline(), "document state matches baseline despite redo branch");

    stack.redo();
    expectTrue(!stack.isAtBaseline(), "redo past baseline undo depth marks unsaved");
}

void testUndoStackCoalesceDoesNotBumpDirtyRevision() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    expectTrue(stack.dirtyRevision() == 1u, "first execute bumps dirty revision");

    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 3, 6, "drag"));
    expectTrue(stack.dirtyRevision() == 1u, "coalesce guards skip revision bump while dirty");
    expectTrue(stack.isDirty(), "coalesce keeps stack dirty");
    expectTrue(stack.coalescedOps() == 2u, "coalesce counter still tracks merges");
}

void testUndoStackIsDirtySince() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    expectTrue(!stack.isDirtySince(0u), "empty stack is not dirty since zero");

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "step"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();
    expectTrue(stack.isDirtySince(0u), "execute dirties since revision zero");
    expectTrue(!stack.isDirtySince(revisionAfterExecute), "not dirty since latest revision");

    stack.markClean();
    stack.undo();
    expectTrue(stack.isDirtySince(revisionAfterExecute), "undo bumps revision past saved mark");
}

void testCommandStackSetBaselineState() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.set_baseline_state();

    expectTrue(stack.isAtBaseline(), "baseline set after first command");
    expectTrue(!stack.isDirty(), "set_baseline_state clears dirty flag");
    expectTrue(!stack.hasUnsavedChanges(), "baseline matches current depth");

    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    expectTrue(stack.hasUnsavedChanges(), "new command moves away from baseline");
    expectTrue(stack.isDirty(), "execute after baseline marks dirty");

    stack.undo();
    expectTrue(stack.isAtBaseline(), "undo back to baseline depth");
    expectTrue(!stack.hasUnsavedChanges(), "undo restores baseline alignment");
    expectTrue(!stack.isDirty(), "baseline dirty guard clears dirty on undo to baseline");
}

void testCommandStackBaselineDirtyGuardOnRedo() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    stack.set_baseline_state();

    stack.undo();
    expectTrue(stack.isDirty(), "undo away from baseline marks dirty");

    stack.redo();
    expectTrue(stack.isAtBaseline(), "redo returns to baseline depth");
    expectTrue(!stack.isDirty(), "baseline dirty guard clears dirty on redo to baseline");
    expectTrue(!stack.hasUnsavedChanges(), "redo to baseline clears unsaved flag");
}

void testCommandStackCoalescedCountSinceBaseline() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    expectTrue(stack.coalescedCount() == 1u, "pre-baseline coalesce tracked globally");
    expectTrue(stack.coalescedCountSinceBaseline() == 1u, "pre-baseline coalesce visible before save");

    stack.set_baseline_state();
    expectTrue(stack.coalescedCountSinceBaseline() == 0u, "baseline resets coalesce counter");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "10,10,10"));
    expectTrue(stack.coalescedCount() == 3u, "total coalesced count accumulates");
    expectTrue(stack.coalescedCountSinceBaseline() == 2u, "post-baseline coalesce tracked separately");
}

void testCommandStackEmptyStackGuardsDirtyRevision() {
    fuse::editor::CommandStack stack;

    expectTrue(stack.dirtyRevision() == 0u, "empty command stack revision starts at zero");

    stack.undo();
    stack.redo();
    stack.clear();

    expectTrue(stack.dirtyRevision() == 0u, "empty-stack undo/redo/clear do not bump revision");
    expectTrue(!stack.isDirty(), "empty-stack guards leave stack clean");
    expectTrue(stack.coalescedCountSinceBaseline() == 0u, "clear on empty resets coalesce baseline");
}

void testCommandStackCoalesceDoesNotBumpDirtyRevision() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    expectTrue(stack.dirtyRevision() == 1u, "first execute bumps dirty revision");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));
    expectTrue(stack.dirtyRevision() == 1u, "coalesce guards skip revision bump while dirty");
    expectTrue(stack.isDirty(), "coalesce keeps stack dirty");
    expectTrue(stack.coalescedCount() == 2u, "coalesce counter still tracks merges");
}

void testCommandStackIsDirtySince() {
    fuse::editor::CommandStack stack;

    expectTrue(!stack.isDirtySince(0u), "empty stack is not dirty since zero");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();
    expectTrue(stack.isDirtySince(0u), "execute dirties since revision zero");
    expectTrue(!stack.isDirtySince(revisionAfterExecute), "not dirty since latest revision");

    stack.markClean();
    stack.undo();
    expectTrue(stack.isDirtySince(revisionAfterExecute), "undo bumps revision past saved mark");
}

void testCommandStackSnapshotBaselineRoundTrip() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    stack.set_baseline_state();

    const fuse::editor::CommandStackSnapshot snapshot = stack.captureSnapshot();
    expectTrue(snapshot.coalescedCount == 1u, "snapshot captures coalescedCount");
    expectTrue(snapshot.coalescedCountAtBaseline == 1u, "snapshot captures coalescedCountAtBaseline");
    expectTrue(snapshot.baselineUndoDepth == 1u, "snapshot captures baseline undo depth");
    expectTrue(snapshot.baselineConfigured, "snapshot captures baseline configured flag");

    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    stack.restoreSnapshot(snapshot);

    expectTrue(stack.coalescedCount() == snapshot.coalescedCount, "restore brings back coalescedCount");
    expectTrue(stack.coalescedCountSinceBaseline() == 0u, "restore brings back coalesce baseline offset");
    expectTrue(stack.isAtBaseline(), "restore brings back baseline alignment");
    expectTrue(!stack.isDirty(), "restore brings back clean dirty flag");
}

void testCommandStackDoubleUndoEmptyGuard() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.undo();
    expectTrue(stack.undoDepth() == 0u, "first undo drains undo branch");
    expectTrue(stack.redoDepth() == 1u, "redo branch preserved after undo");

    stack.undo();
    expectTrue(stack.undoDepth() == 0u, "second undo on empty stack is a no-op");
    expectTrue(stack.redoDepth() == 1u, "redo branch preserved after empty undo guard");

    stack.redo();
    expectTrue(stack.undoDepth() == 1u, "first redo restores command");
    expectTrue(stack.redoDepth() == 0u, "redo branch drained after restore");
    const fuse::u32 appliedAfterRedo = stack.appliedCount();

    stack.redo();
    expectTrue(stack.appliedCount() == appliedAfterRedo, "second redo on empty stack is a no-op");
}

void testCommandStackClearOnEmptyIsNoOp() {
    fuse::editor::CommandStack stack;

    stack.clear();
    stack.clear();

    expectTrue(stack.undoDepth() == 0u, "clear on empty stack stays empty");
    expectTrue(!stack.isDirty(), "clear on empty stack stays clean");
    expectTrue(!stack.isBaselineConfigured(), "clear on empty clears baseline configured flag");
    expectTrue(stack.coalescedCountSinceBaseline() == 0u, "clear on empty resets coalesce baseline");
}

void testCommandStackIsAtBaselineUsesUndoDepthOnly() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.set_baseline_state();

    stack.execute(makeSetPropertyCommand(1u, "sdf.blend_alpha", "0.5"));
    stack.undo();
    expectTrue(stack.redoDepth() == 1u, "undo leaves redo branch after edit");
    expectTrue(stack.isAtBaseline(), "document state matches baseline despite redo branch");

    stack.redo();
    expectTrue(!stack.isAtBaseline(), "redo past baseline undo depth marks unsaved");
}

void testCommandStackSetBaselineOnEmptyStack() {
    fuse::editor::CommandStack stack;

    expectTrue(!stack.isBaselineConfigured(), "empty stack has no baseline configured");

    stack.set_baseline_state();
    expectTrue(stack.isBaselineConfigured(), "set_baseline_state on empty stack configures baseline");
    expectTrue(stack.isAtBaseline(), "empty stack is at baseline after save");
    expectTrue(stack.baselineUndoDepth() == 0u, "empty baseline records zero undo depth");
    expectTrue(stack.baselineRedoDepth() == 0u, "empty baseline records zero redo depth");
    expectTrue(!stack.isDirty(), "set_baseline_state on empty stack clears dirty");
}

void testCommandStackMarkCleanIdempotent() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();

    stack.markClean();
    stack.markClean();
    expectTrue(!stack.isDirty(), "repeated markClean keeps stack clean");
    expectTrue(stack.dirtyRevision() == revisionAfterExecute,
               "repeated markClean does not bump dirty revision");
}

void testCommandStackCoalesceAfterBaselineRevisionGuard() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();
    stack.set_baseline_state();
    expectTrue(!stack.isDirty(), "baseline save clears dirty flag");
    expectTrue(stack.dirtyRevision() == revisionAfterExecute, "baseline save preserves revision");

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "4,5,6"));
    stack.execute(makeSetPropertyCommand(1u, "transform.position", "7,8,9"));
    expectTrue(stack.dirtyRevision() == revisionAfterExecute + 1u,
               "first coalesce after baseline bumps revision once");
    expectTrue(stack.coalescedCountSinceBaseline() == 2u, "post-baseline coalesce tracked separately");
    expectTrue(stack.isDirty(), "coalesce after baseline marks dirty");
}

void testCommandStackIsBaselineConfiguredRoundTrip() {
    fuse::editor::CommandStack stack;

    stack.execute(makeSetPropertyCommand(1u, "transform.position", "1,2,3"));
    stack.set_baseline_state();
    expectTrue(stack.isBaselineConfigured(), "baseline configured after save");
    expectTrue(stack.baselineUndoDepth() == 1u, "baseline undo depth recorded");

    stack.clear();
    expectTrue(!stack.isBaselineConfigured(), "clear resets baseline configured flag");
}

void testUndoStackSetBaselineOnEmptyStack() {
    fuse::editor::UndoStack stack;

    expectTrue(!stack.isBaselineConfigured(), "empty undo stack has no baseline configured");

    stack.set_baseline_state();
    expectTrue(stack.isBaselineConfigured(), "set_baseline_state on empty stack configures baseline");
    expectTrue(stack.isAtBaseline(), "empty undo stack is at baseline after save");
    expectTrue(stack.baselineUndoCount() == 0u, "empty baseline records zero undo count");
    expectTrue(stack.baselineRedoCount() == 0u, "empty baseline records zero redo count");
    expectTrue(!stack.isDirty(), "set_baseline_state on empty stack clears dirty");
}

void testUndoStackMarkCleanIdempotent() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "step"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();

    stack.markClean();
    stack.markClean();
    expectTrue(!stack.isDirty(), "repeated markClean keeps undo stack clean");
    expectTrue(stack.dirtyRevision() == revisionAfterExecute,
               "repeated markClean does not bump dirty revision");
}

void testUndoStackCoalesceAfterBaselineRevisionGuard() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "drag"));
    const fuse::u32 revisionAfterExecute = stack.dirtyRevision();
    stack.set_baseline_state();
    expectTrue(!stack.isDirty(), "baseline save clears dirty flag");
    expectTrue(stack.dirtyRevision() == revisionAfterExecute, "baseline save preserves revision");

    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "drag"));
    stack.execute(std::make_unique<CounterCommand>(counter, 3, 6, "drag"));
    expectTrue(stack.dirtyRevision() == revisionAfterExecute + 1u,
               "first coalesce after baseline bumps revision once");
    expectTrue(stack.coalescedOpsSinceBaseline() == 2u, "post-baseline coalesce tracked separately");
    expectTrue(stack.isDirty(), "coalesce after baseline marks dirty");
}

void testUndoStackIsBaselineConfiguredRoundTrip() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "step"));
    stack.set_baseline_state();
    expectTrue(stack.isBaselineConfigured(), "baseline configured after save");
    expectTrue(stack.baselineUndoCount() == 1u, "baseline undo count recorded");

    stack.clear();
    expectTrue(!stack.isBaselineConfigured(), "clear resets baseline configured flag");
}

void testUndoStackSnapshotCapture() {
    fuse::editor::UndoStack stack;
    int counter = 0;

    stack.execute(std::make_unique<CounterCommand>(counter, 0, 1, "first"));
    stack.execute(std::make_unique<CounterCommand>(counter, 1, 3, "second"));
    stack.markClean();

    const fuse::editor::UndoStackSnapshot snapshot = stack.captureSnapshot();
    expectTrue(snapshot.undoCount == 2u, "snapshot captures undo depth");
    expectTrue(snapshot.redoCount == 0u, "snapshot captures empty redo branch");
    expectTrue(snapshot.undoDescriptions.size() == 2u, "snapshot records undo descriptions");
    expectTrue(snapshot.undoDescriptions[0] == "first", "snapshot preserves undo order");
    expectTrue(snapshot.undoDescriptions[1] == "second", "snapshot preserves undo order");
    expectTrue(!snapshot.dirty, "snapshot captures clean dirty flag");

    stack.undo();
    stack.restoreSnapshot(snapshot);
    expectTrue(stack.undoCount() == 2u, "restore rewinds depth via redo");
    expectTrue(counter == 3, "restore replays undone command");
    expectTrue(!stack.isDirty(), "restore brings back dirty flag");
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
    testCommandStackEmptyStackNoOps();
    testUndoStackEmptyStackNoOps();
    testCommandStackPushClearsRedoBranch();
    testCommandStackCoalesceUndoRestoresFirstValue();
    testCommandStackPeekUndoRedo();
    testCommandStackEvictedCount();
    testCommandStackPreservesCallerBaseline();
    testCommandStackPushWithBeforeBaseline();
    testCommandStackUndoPostsInversePending();
    testCommandStackStackDepthRoundTrip();
    testUndoStackDirtyTracking();
    testUndoStackPushAlias();
    testUndoStackCoalescedOps();
    testUndoStackSetBaselineState();
    testUndoStackBaselineDirtyGuardOnRedo();
    testUndoStackCoalescedOpsSinceBaseline();
    testUndoStackClearOnEmptyIsNoOp();
    testUndoStackDoubleUndoEmptyGuard();
    testUndoStackEmptyStackGuardsDirtyRevision();
    testUndoStackPushClearsRedoBranch();
    testUndoStackIsAtBaselineUsesUndoDepthOnly();
    testUndoStackCoalesceDoesNotBumpDirtyRevision();
    testUndoStackIsDirtySince();
    testUndoStackSnapshotBaselineRoundTrip();
    testUndoStackSnapshotCapture();
    testCommandStackSetBaselineState();
    testCommandStackBaselineDirtyGuardOnRedo();
    testCommandStackCoalescedCountSinceBaseline();
    testCommandStackEmptyStackGuardsDirtyRevision();
    testCommandStackCoalesceDoesNotBumpDirtyRevision();
    testCommandStackIsDirtySince();
    testCommandStackSnapshotBaselineRoundTrip();
    testCommandStackDoubleUndoEmptyGuard();
    testCommandStackClearOnEmptyIsNoOp();
    testCommandStackIsAtBaselineUsesUndoDepthOnly();
    testCommandStackSetBaselineOnEmptyStack();
    testCommandStackMarkCleanIdempotent();
    testCommandStackCoalesceAfterBaselineRevisionGuard();
    testCommandStackIsBaselineConfiguredRoundTrip();
    testUndoStackSetBaselineOnEmptyStack();
    testUndoStackMarkCleanIdempotent();
    testUndoStackCoalesceAfterBaselineRevisionGuard();
    testUndoStackIsBaselineConfiguredRoundTrip();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_command_stack_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_command_stack_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
