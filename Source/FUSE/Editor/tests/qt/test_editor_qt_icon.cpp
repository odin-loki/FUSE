// Appendix A "Icons": the Qt editor host embeds the FUSE app icon (16/32/48/256 px) and installs it
// as the application window icon + desktop-entry id. Runs on the offscreen QPA platform.
#include "app_icon.hpp"
#include <fuse/editor/icon_px.hpp>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QWidget>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void testIconPxOk() {
    expect(fuse::editor::icon_px_ok(8), "icon_px_ok(8)");
    expect(fuse::editor::icon_px_ok(32), "icon_px_ok(32)");
    expect(fuse::editor::icon_px_ok(256), "icon_px_ok(256)");
    expect(!fuse::editor::icon_px_ok(0), "!icon_px_ok(0)");
    expect(!fuse::editor::icon_px_ok(12), "!icon_px_ok(12)");
    expect(!fuse::editor::icon_px_ok(512), "!icon_px_ok(512)");
}

} // namespace

int main(int argc, char** argv) {
    const int failuresBeforeIconPx = failures;
    testIconPxOk();
    if (failures != failuresBeforeIconPx) {
        std::printf("fuse_editor_qt_icon: %d failure(s)\n", failures);
        return 1;
    }
    QApplication app(argc, argv);
    using namespace fuse::editor::qt;

    for (const int size : kAppIconSizes) {
        const QString path = appIconResource(size);
        QImage img(path);
        const bool sized = !img.isNull() && img.width() == size && img.height() == size && img.hasAlphaChannel();
        std::printf("  %s -> %dx%d\n", qPrintable(path), img.width(), img.height());
        expect(sized, "embedded icon raster present at its nominal size with alpha");
        if (sized) {
            // Corners are transparent (rounded tile); the centre is the white fusion core.
            expect(qAlpha(img.pixel(0, 0)) == 0, "  corner transparent");
            const QColor c = img.pixelColor(size / 2, size / 2);
            expect(c.red() > 200 && c.green() > 200 && c.blue() > 200 && c.alpha() == 255, "  centre is the fusion core");
        }
    }

    applyApplicationIdentity(app);
    const QIcon appIcon = QApplication::windowIcon();
    expect(!appIcon.isNull(), "QApplication::windowIcon() set");
    const QList<QSize> sizes = appIcon.availableSizes();
    for (const int size : kAppIconSizes) {
        expect(sizes.contains(QSize(size, size)), "application icon offers every embedded size");
    }
    expect(QGuiApplication::desktopFileName() == QStringLiteral("fuse"), "desktop file name is 'fuse' (fuse.desktop)");

    QWidget window;
    expect(!window.windowIcon().isNull() && window.windowIcon().cacheKey() == appIcon.cacheKey(),
           "top-level editor windows inherit the FUSE icon");

    std::printf("fuse_editor_qt_icon: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

#include <fuse/editor/icon_size.hpp>

namespace {

void testSnapIconSize() {
    expect(fuse::editor::snap_icon_size(20) == 24, "snap_icon_size(20) == 24");
    expect(fuse::editor::snap_icon_size(15) == 16, "snap_icon_size(15) == 16");
    expect(fuse::editor::snap_icon_size(40) == 32, "snap_icon_size(40) == 32");
    expect(fuse::editor::snap_icon_size(28) == 32, "snap_icon_size(28) == 32");
}

// Runs before main(); failures accumulate in the counter main() already reports.
struct SnapIconSizeRegistration {
    SnapIconSizeRegistration() { testSnapIconSize(); }
};

[[maybe_unused]] const SnapIconSizeRegistration g_snapIconSizeRegistration{};

} // namespace
