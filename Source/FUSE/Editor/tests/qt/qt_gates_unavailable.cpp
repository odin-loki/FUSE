// Stand-in for fuse_editor_qt_gates when the Qt 6 editor host is not configured (Qt6 Widgets not
// found, FUSE_BUILD_EDITOR=OFF or a mobile profile): every "gate;qt" test reports a clean skip.
#include <cstdio>

int main(int argc, char** argv) {
    std::printf("SKIP %s: Qt 6 editor host not built (need Qt6 Widgets + FUSE_BUILD_EDITOR=ON)\n",
                argc > 1 ? argv[1] : "qt gate");
    return 77;
}
