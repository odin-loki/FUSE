#pragma once

#include <fuse/editor/editor_host.hpp>

#include <QWidget>

namespace fuse::editor::qt {

/// Embedded runtime viewport hook surface (U6) — posts resize commands to the game thread.
class ViewportPlaceholderWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ViewportPlaceholderWidget(EditorHost& host, QWidget* parent = nullptr);

    void setProjectLabel(const QString& projectName);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void postViewportResize();
    void postVulkanSurfaceHandoffStub();

    EditorHost& m_host;
    QString m_projectName;
};

} // namespace fuse::editor::qt
