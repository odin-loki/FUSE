#include "viewport_placeholder_widget.hpp"
#include "viewport_qt_vulkan_surface.hpp"

#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/editor/command_queue.hpp>

#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>

#include <string>

namespace fuse::editor::qt {

ViewportPlaceholderWidget::ViewportPlaceholderWidget(EditorHost& host, QWidget* parent)
    : QWidget(parent), m_host(host) {
    setMinimumSize(640, 360);
    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(32, 36, 44));
    setPalette(palette);
}

void ViewportPlaceholderWidget::setProjectLabel(const QString& projectName) {
    m_projectName = projectName;
    update();
}

void ViewportPlaceholderWidget::postViewportResize() {
    const QSize size = this->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }

    EditorCommand widthCmd;
    widthCmd.kind = CommandKind::SetProperty;
    widthCmd.propertyName = "viewport.width";
    widthCmd.propertyValue = std::to_string(static_cast<unsigned>(size.width()));
    m_host.postFromUi(std::move(widthCmd));

    EditorCommand heightCmd;
    heightCmd.kind = CommandKind::SetProperty;
    heightCmd.propertyName = "viewport.height";
    heightCmd.propertyValue = std::to_string(static_cast<unsigned>(size.height()));
    m_host.postFromUi(std::move(heightCmd));
}

void ViewportPlaceholderWidget::postVulkanSurfaceHandoffStub() {
    const QSize size = this->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }

    postViewportResize();

    ViewportQtVulkanSurface qtSurface;
    void* surfaceHandle = reinterpret_cast<void*>(winId());
    bool qtStubSurface = true;

#if defined(FUSE_EDITOR_HAS_QT_VULKAN)
    QWindow* surfaceWindow = window() ? window()->windowHandle() : nullptr;
    if (qtSurface.initialize(surfaceWindow)) {
        surfaceHandle = qtSurface.nativeSurface();
        qtStubSurface = false;
    }
#endif

    EditorCommand stubCmd;
    stubCmd.kind = CommandKind::SetProperty;
    stubCmd.propertyName = "viewport.vk_surface_qt_stub";
    stubCmd.propertyValue = qtStubSurface ? "1" : "0";
    m_host.postFromUi(std::move(stubCmd));

    EditorCommand surfaceCmd;
    surfaceCmd.kind = CommandKind::SetProperty;
    surfaceCmd.propertyName = "viewport.vk_surface_handle";
    surfaceCmd.propertyValue = std::to_string(static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(surfaceHandle)));
    m_host.postFromUi(std::move(surfaceCmd));
}

void ViewportPlaceholderWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    postVulkanSurfaceHandoffStub();
}

void ViewportPlaceholderWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    postViewportResize();
}

void ViewportPlaceholderWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));

    const RuntimeViewportHook& hook = m_host.runtimeViewport();
    const QString project = m_projectName.isEmpty() ? tr("(none)") : m_projectName;

    painter.setPen(QColor(180, 190, 210));
    painter.drawText(
        rect().adjusted(16, 16, -16, -80),
        Qt::AlignCenter,
        tr("Runtime viewport hook\n\nProject: %1\nSize: %2 × %3\nRuntime ticks: %4\nEmbedded: %5")
            .arg(project)
            .arg(hook.panel().width())
            .arg(hook.panel().height())
            .arg(hook.runtimeTickCount())
            .arg(hook.isEmbedded() ? tr("yes") : tr("no")));

    const fuse::cinematics::SeqScrubPreview& seqPreview = m_host.cinematicsSeqScrubPreview();
    const QRect overlayRect = rect().adjusted(16, rect().height() - 72, -16, -16);
    painter.fillRect(overlayRect, QColor(24, 28, 36, 220));
    painter.setPen(QColor(120, 180, 255));
    painter.drawRect(overlayRect);
    if (seqPreview.valid) {
        painter.setPen(QColor(210, 220, 235));
        painter.drawText(
            overlayRect.adjusted(8, 8, -8, -8),
            Qt::AlignLeft | Qt::AlignVCenter,
            tr("Seq overlay @ %1 ms — mount %2 yaw %3° sprite (%4, %5)")
                .arg(static_cast<qulonglong>(seqPreview.time_ms))
                .arg(QString::fromStdString(seqPreview.mount_point))
                .arg(seqPreview.mount_yaw_deg, 0, 'f', 1)
                .arg(seqPreview.sprite_x, 0, 'f', 1)
                .arg(seqPreview.sprite_y, 0, 'f', 1));
    } else {
        painter.setPen(QColor(140, 150, 165));
        painter.drawText(overlayRect.adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignVCenter,
                         tr("Seq overlay stub — scrub .seq via CinematicsSeqImport"));
    }
}

} // namespace fuse::editor::qt
