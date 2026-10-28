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

#include "shipping_qt.hpp"

// Line-level shipping Qt include check. The FUSE_HAS_QT ban above is unchanged.
static constexpr bool test_shipping_qt_include() {
    return fuse::shipping_qt_include("#include <QWidget>") &&
           !fuse::shipping_qt_include("#include <vector>") &&
           !fuse::shipping_qt_include("// #include <QWidget>") &&
           !fuse::shipping_qt_include("  // #include <QWidget>") &&
           !fuse::shipping_qt_include("/* #include <QWidget> */") &&
           fuse::shipping_qt_include("\t#include <QWidget>") &&
           fuse::shipping_qt_include("#include <QString>") &&
           fuse::shipping_qt_include("#include <QtCore>") &&
           fuse::shipping_qt_include("#include <QtGui>") &&
           fuse::shipping_qt_include("#include <QtGui/QWindow>") &&
           fuse::shipping_qt_include("#include <QtCore/qstring.h>") &&
           fuse::shipping_qt_include("#include \"qapplication.h\"") &&
           fuse::shipping_qt_include("#include <qt/qapplication.h>") &&
           !fuse::shipping_qt_include("const char* label = \"Qt\";") &&
           !fuse::shipping_qt_include("#include <vector> // Qt") &&
           !fuse::shipping_qt_include("#include \"quat.hpp\"") &&
           !fuse::shipping_qt_include("#include \"notqt.h\"");
}

static_assert(test_shipping_qt_include(), "test_shipping_qt_include");
