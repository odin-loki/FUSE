#include "seq_preview_pane_widget.hpp"

namespace fuse::editor::qt {

SeqPreviewPaneWidget::SeqPreviewPaneWidget(EditorHost& host, QWidget* parent)
    : QWidget(parent), m_host(host), m_seqImport(host) {}

void SeqPreviewPaneWidget::wireEmbeddedOutpostIntro(fuse::cinematics::TimelineMs initialTimeMs) {
    m_seqImport.wirePreviewPaneToHost(initialTimeMs);
}

void SeqPreviewPaneWidget::scrubToMs(fuse::cinematics::TimelineMs timeMs) {
    if (m_seqImport.postScrubPreviewAtMs(timeMs)) {
        ++m_scrubPostCount;
    }
}

void SeqPreviewPaneWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_seqImport.importCount() == 0u) {
        wireEmbeddedOutpostIntro();
    }
}

} // namespace fuse::editor::qt
