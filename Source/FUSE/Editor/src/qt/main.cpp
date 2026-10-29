#include "app_icon.hpp"
#include "editor_theme.hpp"
#include "main_window.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <fuse/core/init.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QTimer>

int main(int argc, char* argv[]) {
    fuse::core::initialize();

    QApplication app(argc, argv);
    QApplication::setApplicationName("fuse_editor");
    QApplication::setApplicationVersion("0.1.0");
    fuse::editor::qt::applyApplicationIdentity(app);
    fuse::editor::qt::applyEditorTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription("FUSE Qt 6 editor shell (desktop-only, U6/WP-08)");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption samplesOption(
        QStringList() << "s" << "samples",
        "Root directory containing unification demo projects.",
        "path",
        QDir(QString::fromUtf8(qgetenv("FUSE_SAMPLES_ROOT"))).absolutePath());
    parser.addOption(samplesOption);
    QCommandLineOption projectOption(QStringList() << "p" << "project",
                                     "Project directory (project.json) to open on launch.", "dir");
    parser.addOption(projectOption);
    QCommandLineOption screenshotOption(
        QStringList() << "screenshot",
        "Grab the editor window to this PNG after --screenshot-delay ms, then quit.", "png");
    parser.addOption(screenshotOption);
    QCommandLineOption screenshotDelayOption(QStringList() << "screenshot-delay",
                                             "Delay before --screenshot (ms).", "ms", "2500");
    parser.addOption(screenshotDelayOption);
    QCommandLineOption softwareViewportOption(
        QStringList() << "software-viewport",
        "Draw the viewport with the software placeholder instead of the embedded Vulkan window.");
    parser.addOption(softwareViewportOption);
    QCommandLineOption selectOption(
        QStringList() << "select",
        "Select an entity after the project opens: 'new' (SDF sphere in front of the camera), 'cube' (Create Cube) or an entity index.",
        "spec");
    parser.addOption(selectOption);
    QCommandLineOption spawnOption(QStringList() << "spawn",
                                   "Spawn content after the project opens: 'terrain' (sculpted Terrain at the origin), "
                                   "'cube' or 'sphere'; the new entity is selected.",
                                   "kind");
    parser.addOption(spawnOption);
    parser.process(app);

    QString samplesRoot = parser.value(samplesOption);
    if (samplesRoot.isEmpty()) {
        samplesRoot = QDir(QCoreApplication::applicationDirPath())
                            .absoluteFilePath("../../../../Samples/unification");
    }

    fuse::editor::qt::MainWindow::Options options;
    options.embeddedVulkanViewport = !parser.isSet(softwareViewportOption);
    // Screenshots use the default layout; interactive sessions keep the user's dock layout.
    options.persistLayout = !parser.isSet(screenshotOption);
    fuse::editor::qt::MainWindow window(samplesRoot, options);
    window.show();
    if (parser.isSet(projectOption)) {
        window.openProject(QFileInfo(parser.value(projectOption)).absoluteFilePath());
    }
    if (parser.isSet(selectOption) || parser.isSet(spawnOption)) {
        QString spec = parser.isSet(spawnOption) ? parser.value(spawnOption) : parser.value(selectOption);
        if (spec.compare("sphere", Qt::CaseInsensitive) == 0) spec = QStringLiteral("new");
        QTimer::singleShot(parser.value(screenshotDelayOption).toInt() / 2, &window, [&window, spec]() {
            if (!window.selectForScreenshot(spec)) {
                qWarning("--select %s: nothing selected", qPrintable(spec));
            }
        });
    }
    if (parser.isSet(screenshotOption)) {
        const QString shotPath = parser.value(screenshotOption);
        QTimer::singleShot(parser.value(screenshotDelayOption).toInt(), &window, [&window, shotPath]() {
            window.refreshPanels();
            // Screen pixels first: QWidget::grab() re-renders widgets and cannot see the native Vulkan
            // viewport (it would show the placeholder background under the overlay).
            QPixmap shot;
            if (QScreen* screen = window.screen(); screen != nullptr && QGuiApplication::platformName() != "offscreen") {
                shot = screen->grabWindow(window.winId());
            }
            if (shot.isNull()) {
                shot = window.grab();
            }
            const bool saved = shot.save(shotPath, "PNG");
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    const int code = app.exec();
    fuse::core::shutdown();
    return code;
}
