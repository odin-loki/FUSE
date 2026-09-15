#include "project_hub_widget.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace fuse::editor::qt {

QStringList discoverProjectDirectories(const QString& samplesRoot) {
    QStringList projects;
    const QDir root(samplesRoot);
    if (!root.exists()) {
        return projects;
    }

    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        if (QFileInfo::exists(entry.absoluteFilePath() + "/project.json")) {
            projects.push_back(entry.absoluteFilePath());
        }
    }
    projects.sort(Qt::CaseInsensitive);
    return projects;
}

ProjectHubWidget::ProjectHubWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Projects"), this);
    title->setStyleSheet("font-weight: bold;");
    layout->addWidget(title);

    m_projectList = new QListWidget(this);
    layout->addWidget(m_projectList, 1);

    m_openButton = new QPushButton(tr("Open Project"), this);
    connect(m_openButton, &QPushButton::clicked, this, &ProjectHubWidget::onOpenClicked);
    layout->addWidget(m_openButton);

    connect(m_projectList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        onOpenClicked();
    });
}

void ProjectHubWidget::setSamplesRoot(const QString& samplesRoot) {
    m_samplesRoot = samplesRoot;
    refreshProjectList();
}

QString ProjectHubWidget::selectedProjectPath() const {
    const QListWidgetItem* item = m_projectList->currentItem();
    if (item == nullptr) {
        return {};
    }
    return item->data(Qt::UserRole).toString();
}

void ProjectHubWidget::refreshProjectList() {
    m_projectList->clear();
    const QStringList projects = discoverProjectDirectories(m_samplesRoot);
    for (const QString& path : projects) {
        const QFileInfo info(path);
        auto* item = new QListWidgetItem(info.fileName());
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        m_projectList->addItem(item);
    }
    if (m_projectList->count() > 0) {
        m_projectList->setCurrentRow(0);
    }
}

void ProjectHubWidget::onOpenClicked() {
    const QString path = selectedProjectPath();
    if (!path.isEmpty()) {
        emit projectOpenRequested(path);
    }
}

} // namespace fuse::editor::qt
