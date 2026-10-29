#include "app_icon.hpp"
#include "editor_theme.hpp"
#include "main_window.hpp"

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
    if (parser.isSet(screenshotOption)) {
        const QString shotPath = parser.value(screenshotOption);
        QTimer::singleShot(parser.value(screenshotDelayOption).toInt(), &window, [&window, shotPath]() {
            window.refreshPanels();
            const bool saved = window.grab().save(shotPath, "PNG");
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    const int code = app.exec();
    fuse::core::shutdown();
    return code;
}
