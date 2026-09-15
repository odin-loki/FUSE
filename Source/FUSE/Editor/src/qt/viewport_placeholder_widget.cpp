#include "viewport_placeholder_widget.hpp"

#include <QColor>
#include <QPainter>
#include <QPalette>

namespace fuse::editor::qt {

ViewportPlaceholderWidget::ViewportPlaceholderWidget(QWidget* parent) : QWidget(parent) {
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

void ViewportPlaceholderWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));

    painter.setPen(QColor(180, 190, 210));
    painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter,
        tr("Viewport placeholder\n\nProject: %1\n\nEmbedded runtime + GPU viewport follow in U6+")
            .arg(m_projectName.isEmpty() ? tr("(none)") : m_projectName));
}

} // namespace fuse::editor::qt
