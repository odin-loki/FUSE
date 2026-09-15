#pragma once

#include <QWidget>

namespace fuse::editor::qt {

/// Placeholder viewport pane until Vulkan/Metal embed lands (U6 follow-up).
class ViewportPlaceholderWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ViewportPlaceholderWidget(QWidget* parent = nullptr);

    void setProjectLabel(const QString& projectName);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_projectName;
};

} // namespace fuse::editor::qt
