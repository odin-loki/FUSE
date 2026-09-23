// Master plan Appendix A (rename checklist): default window title "FUSE".
// A default-constructed WindowDesc and a default (headless stub) Window both report "FUSE".

#include <fuse/platform/window.hpp>

#include <cstdio>
#include <string_view>

namespace {

int g_failures = 0;

void expectTitle(const char* actual, const char* what) {
    const std::string_view got = actual != nullptr ? std::string_view(actual) : std::string_view("<null>");
    if (got != "FUSE") {
        std::fprintf(stderr, "FAIL: %s title is \"%.*s\", expected \"FUSE\"\n", what, int(got.size()), got.data());
        ++g_failures;
    }
}

} // namespace

int main() {
    const fuse::platform::WindowDesc desc{};
    expectTitle(desc.title, "WindowDesc{}");

    fuse::platform::Window window;
    expectTitle(window.description().title, "Window{}.description()");

    // Self-check: the comparison rejects a non-FUSE title.
    fuse::platform::WindowDesc renamed{};
    renamed.title = "Torque 3D";
    if (std::string_view(renamed.title) == "FUSE") {
        std::fprintf(stderr, "FAIL: self-check did not detect a non-FUSE title\n");
        return 1;
    }

    if (g_failures == 0) {
        std::printf("default window title is \"FUSE\"\n");
    }
    return g_failures == 0 ? 0 : 1;
}
