#include "property_pane_widget.hpp"

#include <fuse/editor/editor_host.hpp>

namespace fuse::editor::qt {

PropertyPaneWidget::PropertyPaneWidget(FeaturePaneBridge& bridge, QWidget* parent)
    : QWidget(parent), m_bridge(bridge) {
    auto* layout = new QVBoxLayout(this);

    m_summaryLabel = new QLabel(tr("Property pane — no selection"), this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    m_playButton = new QPushButton(tr("Play (PIE)"), this);
    m_stopButton = new QPushButton(tr("Stop"), this);
    layout->addWidget(m_playButton);
    layout->addWidget(m_stopButton);
    layout->addStretch(1);

    connect(m_playButton, &QPushButton::clicked, this, &PropertyPaneWidget::onPlayClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &PropertyPaneWidget::onStopClicked);

    refresh();
}

void PropertyPaneWidget::refresh() {
    m_bridge.syncPropertyPane();
    const EditorHost& host = m_bridge.host();
    const PropertyInspector& inspector = m_bridge.propertyInspector();

    QString summary = tr("Project: %1 | Playing: %2 | Sections: %3")
                          .arg(QString::fromStdString(host.loadedProject()))
                          .arg(host.editorState().playing ? tr("yes") : tr("no"))
                          .arg(inspector.hasSelection() ? static_cast<int>(inspector.sections().size()) : 0);
    m_summaryLabel->setText(summary);
}

void PropertyPaneWidget::onPlayClicked() {
    m_bridge.postPlayRequested();
    refresh();
}

void PropertyPaneWidget::onStopClicked() {
    m_bridge.postStopRequested();
    refresh();
}

} // namespace fuse::editor::qt
