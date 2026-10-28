// Stand-in for fuse_editor_qt_gates when the Qt 6 editor host is not configured (Qt6 Widgets not
// found, FUSE_BUILD_EDITOR=OFF or a mobile profile): every "gate;qt" test reports a clean skip.
#include "skip_status.hpp"

#include <cstdio>

namespace {

/// No allocation. Status 77 is the skip code; 0, 76, and 78 are not.
int testSkipStatusOk() {
    if (!fuse::editor::skip_status_ok(77) || fuse::editor::skip_status_ok(0) ||
        fuse::editor::skip_status_ok(76) || fuse::editor::skip_status_ok(78)) {
        std::fprintf(stderr, "FAIL: testSkipStatusOk\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (const int skipStatusRc = testSkipStatusOk()) {
        return skipStatusRc;
    }
    std::printf("SKIP %s: Qt 6 editor host not built (need Qt6 Widgets + FUSE_BUILD_EDITOR=ON)\n",
                argc > 1 ? argv[1] : "qt gate");
    return 77;
}
