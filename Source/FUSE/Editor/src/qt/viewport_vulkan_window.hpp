#pragma once

#include <QPointer>
#include <QWindow>

class QWidget;

namespace fuse::editor::qt {

/// Native Vulkan child window of the editor viewport (`surfaceType() == VulkanSurface`), embedded
/// with `QWidget::createWindowContainer`. The FUSE renderer presents into the surface Qt creates on
/// it; the window itself has no editor logic: key / mouse / wheel / focus events are forwarded to
/// the viewport widget (`target`), so the fly camera, picking and the context menu behave exactly
/// as on the software placeholder. The window covers the widget at (0, 0), so positions map 1:1.
class ViewportVulkanWindow final : public QWindow {
    Q_OBJECT

public:
    explicit ViewportVulkanWindow(QWidget* target);

    [[nodiscard]] quint64 forwardedEventCount() const { return m_forwarded; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;

private:
    void forwardKey(QKeyEvent* event);
    void forwardMouse(QMouseEvent* event);

    QPointer<QWidget> m_target;
    quint64 m_forwarded = 0;
};

} // namespace fuse::editor::qt
