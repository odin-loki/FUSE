#include <fuse/core/init.hpp>
#include <fuse/editor/command_name.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/queue_depth.hpp>

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

void testQueueDepthOk() {
    expectTrue(fuse::editor::queue_depth_ok(1), "depth 1 is accepted");
    expectTrue(fuse::editor::queue_depth_ok(256), "depth 256 is accepted");
    expectTrue(!fuse::editor::queue_depth_ok(0), "depth 0 is rejected");
    expectTrue(!fuse::editor::queue_depth_ok(257), "depth 257 is rejected");
}

void testCommandNameOk() {
    expectTrue(fuse::editor::command_name_ok("Move"), "Move is a command name");
    expectTrue(!fuse::editor::command_name_ok(""), "empty name is rejected");
    expectTrue(!fuse::editor::command_name_ok("1Move"), "name starting with a digit is rejected");
    expectTrue(fuse::editor::command_name_ok("a"), "single ASCII letter is a command name");
    expectTrue(fuse::editor::command_name_ok("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"),
               "48-character name starting with a letter is accepted");
    expectTrue(!fuse::editor::command_name_ok("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"),
               "49-character name is rejected");
}

void testCommandQueueRoundTrip() {
    fuse::editor::CommandQueue queue;
    fuse::editor::EditorCommand cmd;
    cmd.kind = fuse::editor::CommandKind::SetProperty;
    cmd.propertyName = "position";
    cmd.propertyValue = "0 0 0";

    queue.post(cmd);
    expectTrue(queue.pendingCount() == 1u, "posted command pending");

    queue.drain();
    expectTrue(queue.pendingCount() == 0u, "queue drained");
    expectTrue(queue.appliedCount() == 1u, "command applied on game thread");
    expectTrue(queue.lastDrainedBatch().size() == 1u, "last batch retains payload");
    expectTrue(queue.lastDrainedBatch()[0].propertyName == "position", "property name preserved");
    expectTrue(queue.lastDrainedBatch()[0].propertyValue == "0 0 0", "property value preserved");
}

void testCommandQueueMultipleKinds() {
    fuse::editor::CommandQueue queue;

    fuse::editor::EditorCommand setProp;
    setProp.kind = fuse::editor::CommandKind::SetProperty;
    setProp.propertyName = "rotation";
    queue.post(setProp);

    fuse::editor::EditorCommand deleteObj;
    deleteObj.kind = fuse::editor::CommandKind::DeleteObject;
    deleteObj.target = fuse::Handle<fuse::Object>(5u, 1u);
    queue.post(deleteObj);

    fuse::editor::EditorCommand reparent;
    reparent.kind = fuse::editor::CommandKind::ReparentObject;
    reparent.target = fuse::Handle<fuse::Object>(6u, 1u);
    reparent.parent = fuse::Handle<fuse::Object>(1u, 1u);
    queue.post(reparent);

    expectTrue(queue.pendingCount() == 3u, "three commands pending");

    queue.drain();
    expectTrue(queue.pendingCount() == 0u, "batch drained");
    expectTrue(queue.appliedCount() == 3u, "all commands applied");
    expectTrue(queue.lastDrainedBatch().size() == 3u, "batch order preserved");
    expectTrue(queue.lastDrainedBatch()[1].kind == fuse::editor::CommandKind::DeleteObject,
               "delete kind preserved");
    expectTrue(queue.lastDrainedBatch()[2].parent.index() == 1u, "reparent parent handle preserved");
}

void testCommandQueuePreservesPostOrder() {
    fuse::editor::CommandQueue queue;

    for (fuse::u32 i = 0; i < 5u; ++i) {
        fuse::editor::EditorCommand cmd;
        cmd.kind = fuse::editor::CommandKind::SetProperty;
        cmd.propertyName = "seq";
        cmd.propertyValue = std::to_string(i);
        queue.post(std::move(cmd));
    }

    queue.drain();
    expectTrue(queue.lastDrainedBatch().size() == 5u, "five commands in batch");
    for (fuse::u32 i = 0; i < 5u; ++i) {
        expectTrue(queue.lastDrainedBatch()[i].propertyValue == std::to_string(i),
                   "fifo order preserved");
    }
}

} // namespace

int main() {
    const int failuresBeforeQueueDepth = g_failures;
    testQueueDepthOk();
    if (g_failures != failuresBeforeQueueDepth) {
        std::fprintf(stderr, "fuse_editor_api_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testCommandNameOk();
    fuse::core::initialize();
    testCommandQueueRoundTrip();
    testCommandQueueMultipleKinds();
    testCommandQueuePreservesPostOrder();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_api_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_api_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

#include <fuse/editor/command_coalesce.hpp>

#include <limits>

namespace {

void testCommandCoalesceWindow() {
    expectTrue(fuse::editor::commands_coalesce("Move", "Move", 0.1f, 0.5f),
               "Move then Move at 0.1s with window 0.5 coalesces");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Rotate", 0.1f, 0.5f),
               "Move then Rotate does not coalesce");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", 1.0f, 0.5f),
               "elapsed 1.0 with window 0.5 does not coalesce");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", nan, 0.5f),
               "non-finite elapsed does not coalesce");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", 0.1f, nan),
               "non-finite window does not coalesce");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", inf, inf),
               "infinite elapsed and window do not coalesce");
    expectTrue(!fuse::editor::commands_coalesce("", "", 0.1f, 0.5f),
               "empty labels do not coalesce");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", -0.1f, 0.5f),
               "negative elapsed does not coalesce");
    expectTrue(fuse::editor::commands_coalesce("Move", "Move", 0.5f, 0.5f),
               "elapsed equal to the window coalesces");
    expectTrue(!fuse::editor::commands_coalesce("Move", "Move", 0.1f, 0.f),
               "a zero window does not coalesce");
}

// Appended gate: runs before main so the existing failure count still reports it.
struct CommandCoalesceWindowTestHook {
    CommandCoalesceWindowTestHook() { testCommandCoalesceWindow(); }
};

[[maybe_unused]] const CommandCoalesceWindowTestHook g_commandCoalesceWindowTestHook{};

} // namespace
