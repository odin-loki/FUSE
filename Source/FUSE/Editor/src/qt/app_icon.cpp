#include "app_icon.hpp"

#include <QApplication>
#include <QGuiApplication>

namespace fuse::editor::qt {

QString appIconResource(int size) {
    return QStringLiteral(":/fuse/branding/fuse_%1.png").arg(size);
}

QIcon applicationIcon() {
    QIcon icon;
    for (const int size : kAppIconSizes) {
        icon.addFile(appIconResource(size), QSize(size, size));
    }
    return icon;
}

void applyApplicationIdentity(QApplication& app) {
    app.setWindowIcon(applicationIcon());
    QGuiApplication::setDesktopFileName(QStringLiteral("fuse"));
}

} // namespace fuse::editor::qt
