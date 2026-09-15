#include "main_window.hpp"

#include <fuse/core/init.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char* argv[]) {
    fuse::core::initialize();

    QApplication app(argc, argv);
    QApplication::setApplicationName("fuse_editor");
    QApplication::setApplicationVersion("0.1.0");

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
    parser.process(app);

    QString samplesRoot = parser.value(samplesOption);
    if (samplesRoot.isEmpty()) {
        samplesRoot = QDir(QCoreApplication::applicationDirPath())
                            .absoluteFilePath("../../../../Samples/unification");
    }

    fuse::editor::qt::MainWindow window(samplesRoot);
    window.show();

    const int code = app.exec();
    fuse::core::shutdown();
    return code;
}
