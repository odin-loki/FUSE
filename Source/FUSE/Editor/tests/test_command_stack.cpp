#include <fuse/core/init.hpp>
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

} // namespace

int main() {
    fuse::core::initialize();
    testUndoRedoLifo();
    testMergeConsecutiveCommands();
    testSetObjectNameCommand();
    testReparentObjectCommand();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_command_stack_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_command_stack_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
