#include <fuse/core/init.hpp>
#include <fuse/mechanics/toggle_component.hpp>

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

void testToggleComponent() {
    fuse::mechanics::ToggleComponent toggle("lever_switch", false);
    expectTrue(!toggle.state(), "toggle starts off");
    expectTrue(toggle.toggle(), "toggle flips on");
    expectTrue(toggle.state(), "toggle state is on");
    expectTrue(toggle.toggleCount() == 1u, "toggle count increments once");

    toggle.setState(true);
    expectTrue(toggle.toggleCount() == 1u, "setState to same value is no-op");
    toggle.setState(false);
    expectTrue(toggle.toggleCount() == 2u, "setState to new value increments count");
}

} // namespace

int main() {
    fuse::core::initialize();
    testToggleComponent();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_toggle_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_toggle_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
