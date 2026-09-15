#include "main_window.hpp"

#include <fuse/editor/command_queue.hpp>

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace fuse::editor::qt {

MainWindow::MainWindow(const QString& samplesRoot, QWidget* parent)
    : QMainWindow(parent), m_gameThread(&m_host, this) {
    setWindowTitle(tr("FUSE Editor"));
    resize(1024, 720);

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);

    auto* splitter = new QSplitter(Qt::Horizontal, central);
    m_projectHub = new ProjectHubWidget(splitter);
    m_projectHub->setSamplesRoot(samplesRoot);
    m_viewport = new ViewportPlaceholderWidget(splitter);
    splitter->addWidget(m_projectHub);
    splitter->addWidget(m_viewport);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 760});
    layout->addWidget(splitter);
    setCentralWidget(central);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel, 1);

    connect(m_projectHub, &ProjectHubWidget::projectOpenRequested, this,
        &MainWindow::onProjectOpenRequested);

    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatusBar);
    m_statusTimer.start();

    m_gameThread.start();
    refreshStatusBar();
}

MainWindow::~MainWindow() {
    m_statusTimer.stop();
    m_gameThread.quit();
    m_gameThread.wait();
}

void MainWindow::onProjectOpenRequested(const QString& projectDirectory) {
    const QFileInfo info(projectDirectory);
    m_viewport->setProjectLabel(info.fileName());

    EditorCommand cmd;
    cmd.kind = CommandKind::SetProperty;
    cmd.propertyName = "project";
    cmd.propertyValue = info.fileName().toStdString();
    m_host.postFromUi(std::move(cmd));
}

void MainWindow::refreshStatusBar() {
    m_statusLabel->setText(
        tr("Game ticks: %1 | Pending: %2 | Applied: %3")
            .arg(m_host.gameTickCount())
            .arg(m_host.commandQueue().pendingCount())
            .arg(m_host.commandQueue().appliedCount()));
}

} // namespace fuse::editor::qt
