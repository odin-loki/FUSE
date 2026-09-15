#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_host.hpp>

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

void testHostDrainsOnGameTick() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand cmd;
    cmd.kind = fuse::editor::CommandKind::SetProperty;
    cmd.propertyName = "project";
    cmd.propertyValue = "demo_3d_empty";
    host.postFromUi(std::move(cmd));

    expectTrue(host.commandQueue().pendingCount() == 1u, "command pending after UI post");
    expectTrue(host.gameTickCount() == 0u, "no game ticks before drain");

    host.gameTick();

    expectTrue(host.gameTickCount() == 1u, "game tick recorded");
    expectTrue(host.commandQueue().pendingCount() == 0u, "queue drained on game thread");
    expectTrue(host.commandQueue().appliedCount() == 1u, "command applied");
}

void testHostMultipleTicks() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand deleteCmd;
    deleteCmd.kind = fuse::editor::CommandKind::DeleteObject;
    deleteCmd.target = fuse::Handle<fuse::Object>(3u, 1u);
    host.postFromUi(std::move(deleteCmd));

    host.gameTick();
    host.gameTick();

    expectTrue(host.gameTickCount() == 2u, "two game ticks");
    expectTrue(host.commandQueue().appliedCount() == 1u, "single command applied once");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHostDrainsOnGameTick();
    testHostMultipleTicks();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_host_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_host_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
