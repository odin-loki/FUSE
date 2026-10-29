#pragma once

#include <fuse/editor/cinematics_seq_import.hpp>
#include <fuse/editor/editor_host.hpp>

#include <QCheckBox>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

namespace fuse::editor::qt {

/// Qt seq preview pane stub (U5 wave 16) — wires CinematicsSeqImport scrub through EditorHost.
class SeqPreviewPaneWidget final : public QWidget {
    Q_OBJECT

public:
    explicit SeqPreviewPaneWidget(EditorHost& host, QWidget* parent = nullptr);

    CinematicsSeqImport& seqImport() { return m_seqImport; }
    const CinematicsSeqImport& seqImport() const { return m_seqImport; }

    void wireEmbeddedOutpostIntro(fuse::cinematics::TimelineMs initialTimeMs = 2'500);
    void scrubToMs(fuse::cinematics::TimelineMs timeMs);

    [[nodiscard]] u32 scrubPostCount() const { return m_scrubPostCount; }
    [[nodiscard]] const QString& lastPreviewLabel() const { return m_lastPreviewLabel; }

    /// Sequencer <-> Timeline link: when on, the editor timeline playhead scrubs the sequence and the
    /// sequencer's scrub bar moves the timeline playhead (one shared clock).
    void setLinked(bool on);
    [[nodiscard]] bool linked() const { return m_linkBox != nullptr && m_linkBox->isChecked(); }
    /// Called by the Timeline dock (no echo back while following).
    void followTimeline(int ms);
    void setRangeMs(int durationMs);
    [[nodiscard]] int scrubBarMs() const { return m_scrub != nullptr ? m_scrub->value() : 0; }
    [[nodiscard]] QSlider* scrubBar() const { return m_scrub; }
    [[nodiscard]] QCheckBox* linkBox() const { return m_linkBox; }

signals:
    /// The user scrubbed the sequencer while linked: the timeline should follow.
    void scrubbedTo(int ms);

protected:
    void showEvent(QShowEvent* event) override;

private:
    EditorHost& m_host;
    CinematicsSeqImport m_seqImport;
    QLabel* m_previewLabel = nullptr;
    QCheckBox* m_linkBox = nullptr;
    QSlider* m_scrub = nullptr;
    bool m_following = false;
    QString m_lastPreviewLabel;
    u32 m_scrubPostCount = 0;
};

} // namespace fuse::editor::qt
