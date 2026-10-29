#include "seq_preview_pane_widget.hpp"

#include <QHBoxLayout>
#include <QSignalBlocker>

namespace fuse::editor::qt {

SeqPreviewPaneWidget::SeqPreviewPaneWidget(EditorHost& host, QWidget* parent)
    : QWidget(parent), m_host(host), m_seqImport(host) {
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    m_linkBox = new QCheckBox(tr("Link to Timeline"), this);
    m_linkBox->setObjectName(QStringLiteral("fuseSequencerLink"));
    m_linkBox->setToolTip(tr("Share one playhead with the Timeline dock"));
    m_linkBox->setChecked(true);
    m_scrub = new QSlider(Qt::Horizontal, this);
    m_scrub->setObjectName(QStringLiteral("fuseSequencerScrub"));
    m_scrub->setRange(0, 5000);
    m_scrub->setSingleStep(10);
    m_scrub->setPageStep(250);
    row->addWidget(m_linkBox);
    row->addWidget(m_scrub, 1);
    layout->addLayout(row);
    m_previewLabel = new QLabel("Seq preview: (not scrubbed)", this);
    layout->addWidget(m_previewLabel);
    layout->addStretch(1);
    connect(m_scrub, &QSlider::valueChanged, this, [this](int ms) {
        scrubToMs(ms);
        if (!m_following && linked()) {
            emit scrubbedTo(ms);
        }
    });
}

void SeqPreviewPaneWidget::setLinked(bool on) {
    m_linkBox->setChecked(on);
}

void SeqPreviewPaneWidget::setRangeMs(int durationMs) {
    const QSignalBlocker block(m_scrub);
    m_scrub->setRange(0, durationMs > 0 ? durationMs : 1);
}

void SeqPreviewPaneWidget::followTimeline(int ms) {
    if (!linked()) {
        return;
    }
    m_following = true;
    if (ms > m_scrub->maximum()) {
        setRangeMs(ms);
    }
    if (m_scrub->value() == ms) {
        scrubToMs(ms);
    } else {
        m_scrub->setValue(ms); // valueChanged -> scrubToMs; m_following suppresses the echo
    }
    m_following = false;
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
            QString("t=%1ms sprite=(%2,%3) mount=%4 bone=%5 yaw=%6 pitch=%7 roll=%8")
                .arg(sample.time_ms)
                .arg(sample.sprite_x, 0, 'f', 1)
                .arg(sample.sprite_y, 0, 'f', 1)
                .arg(QString::fromStdString(sample.mount_point))
                .arg(QString::fromStdString(sample.bone_name))
                .arg(sample.mount_yaw_deg, 0, 'f', 1)
                .arg(sample.mount_pitch_deg, 0, 'f', 1)
                .arg(sample.mount_roll_deg, 0, 'f', 1);
        m_previewLabel->setText(m_lastPreviewLabel);
    }

    m_seqImport.postViewportSeqPreviewAtMs(timeMs);
}

void SeqPreviewPaneWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_seqImport.importCount() == 0u) {
        wireEmbeddedOutpostIntro();
    }
}

} // namespace fuse::editor::qt
