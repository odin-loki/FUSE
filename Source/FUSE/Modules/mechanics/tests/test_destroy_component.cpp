#include <fuse/core/init.hpp>
#include <fuse/mechanics/destroy_component.hpp>

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

} // namespace

int main() {
    fuse::core::initialize();

    fuse::mechanics::DestroyComponent destroy("lever_destroy");
    expectTrue(destroy.destroyOnTrigger(), "destroy on trigger default true");
    expectTrue(destroy.triggerDestroy(), "first trigger succeeds");
    expectTrue(destroy.triggered(), "destroy marked triggered");
    expectTrue(destroy.destroyCount() == 1u, "destroy count tracked");
    expectTrue(!destroy.triggerDestroy(), "second trigger rejected");

    destroy.setDestroyOnTrigger(false);
    expectTrue(!destroy.triggerDestroy(), "destroy disabled when flag off");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_destroy: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_destroy: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
