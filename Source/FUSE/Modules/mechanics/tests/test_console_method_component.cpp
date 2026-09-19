#include <fuse/mechanics/console_method_component.hpp>
#include <fuse/core/init.hpp>

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

    fuse::mechanics::ConsoleMethodComponent console("test_console");
    bool invoked = false;
    console.registerMethod("toggleLever", [&invoked]() { invoked = true; });

    expectTrue(console.hasMethod("toggleLever"), "console method registered");
    expectTrue(console.invoke("toggleLever"), "console method invokes");
    expectTrue(invoked, "console method callback ran");
    expectTrue(console.invokeCount() == 1u, "invoke count tracked");
    expectTrue(!console.invoke("missing"), "missing method returns false");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_console_method: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_console_method: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
