#include <fuse/mechanics/console_method_component.hpp>
#include <fuse/mechanics/method_name.hpp>
#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testMethodNameOk() {
    expectTrue(fuse::mechanics::method_name_ok("setHealth"), "setHealth is a valid method name");
    expectTrue(!fuse::mechanics::method_name_ok("1set"), "leading digit is an invalid method name");
    expectTrue(!fuse::mechanics::method_name_ok(""), "empty method name is invalid");
    expectTrue(fuse::mechanics::method_name_ok("_setHealth"), "leading underscore is a valid method name");
    expectTrue(fuse::mechanics::method_name_ok("A"), "single letter is a valid method name");
    const std::string maxName(48, 'a');
    expectTrue(fuse::mechanics::method_name_ok(maxName), "48-character method name is valid");
    const std::string tooLong(49, 'a');
    expectTrue(!fuse::mechanics::method_name_ok(tooLong), "method name longer than 48 is invalid");
    expectTrue(!fuse::mechanics::method_name_ok("set-health"), "hyphen is an invalid method name");
}

} // namespace

int main() {
    testMethodNameOk();
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
