#include <fuse/core/init.hpp>
#include <fuse/mechanics/toggle_component.hpp>
#include <fuse/mechanics/toggle_edge.hpp>
#include <fuse/mechanics/toggle_id.hpp>

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

void testToggleIdOk() {
    expectTrue(fuse::mechanics::toggle_id_ok(0), "toggle_id_ok(0) is true");
    expectTrue(fuse::mechanics::toggle_id_ok(255), "toggle_id_ok(255) is true");
    expectTrue(!fuse::mechanics::toggle_id_ok(-1), "toggle_id_ok(-1) is false");
    expectTrue(!fuse::mechanics::toggle_id_ok(256), "toggle_id_ok(256) is false");
}

void testToggleOnPress() {
    expectTrue(fuse::mechanics::toggle_on_press(false, true, false),
               "toggle_on_press(false, true, false) flips off to on");
    expectTrue(fuse::mechanics::toggle_on_press(true, true, true),
               "toggle_on_press(true, true, true) holds while pressed");
    expectTrue(fuse::mechanics::toggle_on_press(true, false, true),
               "toggle_on_press(true, false, true) holds on release");
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
    testToggleIdOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_toggle_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testToggleOnPress();
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
