#pragma once

#include <QString>
#include <QStringList>

#include <QWidget>

class QListWidget;
class QPushButton;

namespace fuse::editor::qt {

/// Desktop project hub — lists discovered `project.json` folders (no scene pointers).
class ProjectHubWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectHubWidget(QWidget* parent = nullptr);

    void setSamplesRoot(const QString& samplesRoot);
    QString selectedProjectPath() const;

signals:
    void projectOpenRequested(const QString& projectDirectory);

private:
    void refreshProjectList();
    void onOpenClicked();

    QString m_samplesRoot;
    QListWidget* m_projectList = nullptr;
    QPushButton* m_openButton = nullptr;
};

QStringList discoverProjectDirectories(const QString& samplesRoot);

} // namespace fuse::editor::qt
