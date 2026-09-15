#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>

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
}

} // namespace

int main() {
    fuse::core::initialize();
    testCommandQueueRoundTrip();
    testCommandQueueMultipleKinds();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_api_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_api_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
