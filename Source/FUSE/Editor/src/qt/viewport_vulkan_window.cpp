#include "viewport_vulkan_window.hpp"

#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>

namespace fuse::editor::qt {

ViewportVulkanWindow::ViewportVulkanWindow(QWidget* target) : m_target(target) {
    setObjectName(QStringLiteral("fuseViewportVulkanWindow"));
    setSurfaceType(QSurface::VulkanSurface);
}

void ViewportVulkanWindow::forwardKey(QKeyEvent* event) {
    if (m_target == nullptr) {
        event->ignore();
        return;
    }
    QKeyEvent copy(event->type(), event->key(), event->modifiers(), event->nativeScanCode(),
                   event->nativeVirtualKey(), event->nativeModifiers(), event->text(), event->isAutoRepeat(),
                   static_cast<quint16>(event->count()), event->device());
    QCoreApplication::sendEvent(m_target, &copy);
    event->setAccepted(copy.isAccepted());
    ++m_forwarded;
}

void ViewportVulkanWindow::forwardMouse(QMouseEvent* event) {
    if (m_target == nullptr) {
        event->ignore();
        return;
    }
    // Same origin as the widget (the container fills it): window-local == widget-local.
    QMouseEvent copy(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                     event->button(), event->buttons(), event->modifiers(), event->pointingDevice());
    QCoreApplication::sendEvent(m_target, &copy);
    event->setAccepted(copy.isAccepted());
    ++m_forwarded;
}

void ViewportVulkanWindow::keyPressEvent(QKeyEvent* event) {
    forwardKey(event);
}

void ViewportVulkanWindow::keyReleaseEvent(QKeyEvent* event) {
    forwardKey(event);
}

void ViewportVulkanWindow::mousePressEvent(QMouseEvent* event) {
    forwardMouse(event);
}

void ViewportVulkanWindow::mouseReleaseEvent(QMouseEvent* event) {
    forwardMouse(event);
}

void ViewportVulkanWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    forwardMouse(event);
}

void ViewportVulkanWindow::mouseMoveEvent(QMouseEvent* event) {
    forwardMouse(event);
}

void ViewportVulkanWindow::wheelEvent(QWheelEvent* event) {
    if (m_target == nullptr) {
        event->ignore();
        return;
    }
    QWheelEvent copy(event->position(), event->globalPosition(), event->pixelDelta(), event->angleDelta(),
                     event->buttons(), event->modifiers(), event->phase(), event->inverted(), event->source(),
                     event->pointingDevice());
    QCoreApplication::sendEvent(m_target, &copy);
    event->setAccepted(copy.isAccepted());
    ++m_forwarded;
}

void ViewportVulkanWindow::focusOutEvent(QFocusEvent* event) {
    // Key releases may land elsewhere once this window loses focus: release held camera input
    // unless keyboard focus simply moved to the viewport widget itself (click-to-focus).
    if (m_target != nullptr && !m_target->hasFocus()) {
        QFocusEvent out(QEvent::FocusOut, event->reason());
        QCoreApplication::sendEvent(m_target, &out);
    }
    QWindow::focusOutEvent(event);
}

void ViewportVulkanWindow::exposeEvent(QExposeEvent* event) {
    // Frames come from the FUSE game thread; nothing to paint on the UI thread.
    QWindow::exposeEvent(event);
}

} // namespace fuse::editor::qt
