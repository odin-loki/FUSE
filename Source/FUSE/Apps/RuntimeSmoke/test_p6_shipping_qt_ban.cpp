#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>

#if defined(FUSE_HAS_QT) && FUSE_HAS_QT
#error "shipping runtime must not see Qt"
#endif

#ifndef FUSE_HAS_QT
#define FUSE_HAS_QT 0
#endif

static_assert(FUSE_HAS_QT != 1, "shipping runtime must not see Qt");

int main() {
    if (!fuse::core::initialize()) {
        std::fprintf(stderr, "fuse_p6_shipping_qt_ban: initialize failed\n");
        return EXIT_FAILURE;
    }
    fuse::core::shutdown();
    std::printf("fuse_p6_shipping_qt_ban: PASS\n");
    return EXIT_SUCCESS;
}
