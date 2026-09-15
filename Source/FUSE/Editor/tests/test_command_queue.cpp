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

} // namespace

int main() {
    fuse::core::initialize();
    testCommandQueueRoundTrip();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_api_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_api_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
