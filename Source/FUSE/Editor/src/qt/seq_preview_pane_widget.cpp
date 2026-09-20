#include "seq_preview_pane_widget.hpp"

namespace fuse::editor::qt {

SeqPreviewPaneWidget::SeqPreviewPaneWidget(EditorHost& host, QWidget* parent)
    : QWidget(parent), m_host(host), m_seqImport(host) {
    auto* layout = new QVBoxLayout(this);
    m_previewLabel = new QLabel("Seq preview: (not scrubbed)", this);
    layout->addWidget(m_previewLabel);
}

void SeqPreviewPaneWidget::wireEmbeddedOutpostIntro(fuse::cinematics::TimelineMs initialTimeMs) {
    m_seqImport.wirePreviewPaneToHost(initialTimeMs);
}

void SeqPreviewPaneWidget::scrubToMs(fuse::cinematics::TimelineMs timeMs) {
    if (m_seqImport.postScrubPreviewAtMs(timeMs)) {
        ++m_scrubPostCount;
    }

    const SeqPreviewPaneSample sample = m_seqImport.previewPaneSampleAtMs(timeMs);
    if (sample.valid && m_previewLabel != nullptr) {
        m_lastPreviewLabel =
            QString("t=%1ms sprite=(%2,%3) mount=%4 bone=%5 yaw=%6")
                .arg(sample.time_ms)
                .arg(sample.sprite_x, 0, 'f', 1)
                .arg(sample.sprite_y, 0, 'f', 1)
                .arg(QString::fromStdString(sample.mount_point))
                .arg(QString::fromStdString(sample.bone_name))
                .arg(sample.mount_yaw_deg, 0, 'f', 1);
        m_previewLabel->setText(m_lastPreviewLabel);
    }
}

void SeqPreviewPaneWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_seqImport.importCount() == 0u) {
        wireEmbeddedOutpostIntro();
    }
}

} // namespace fuse::editor::qt
